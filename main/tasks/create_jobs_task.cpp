#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mining.h"

#include "global_state.h"
#include "create_jobs_task.h"
#include "can_sender.h"
#include "can_master_task.h"

#include "boards/board.h"
#include "macros.h"
#include "system.h"

#define PRIMARY 0
#define SECONDARY 1

static const char *TAG = "create_jobs_task";

pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t current_stratum_job_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================================
// MiningInfoBase - abstract interface for protocol-agnostic job construction
// ============================================================================

MiningInfoBase::~MiningInfoBase() {}

// ============================================================================
// MiningInfoV1 - Stratum V1 job construction
// ============================================================================

class MiningInfoV1 : public MiningInfoBase {
  public:
    mining_notify *current_job = nullptr;

    char *extranonce_str = nullptr;
    int extranonce_2_len = 0;

    char *next_extranonce_str = nullptr;
    int next_extranonce_2_len = 0;

    uint32_t stratum_difficulty = 8192;
    uint32_t active_stratum_difficulty = 8192;
    uint32_t version_mask = 0;

  public:
    MiningInfoV1()
    {
        current_job = (mining_notify *) CALLOC(1, sizeof(mining_notify));
    }

    ~MiningInfoV1() override
    {
        safe_free(extranonce_str);
        safe_free(next_extranonce_str);
        if (current_job) {
            safe_free(current_job->job_id);
            safe_free(current_job->coinbase_1);
            safe_free(current_job->coinbase_2);
            free(current_job);
        }
    }

    // --- MiningInfoBase interface overrides ---

    // 32-bit imza eşitlemesi
    bm_job* buildBmJob(uint32_t extranonce_2, int pool_id, uint32_t asic_diff) override
    {
        return buildBmJob((uint64_t)extranonce_2, pool_id, asic_diff);
    }

    // 64-bit imza (4 bayt ve 8 bayt tam destekli)
    bm_job* buildBmJob(uint64_t extranonce_2, int pool_id, uint32_t asic_diff) override
    {
        // 4 veya 8 bayt durumuna göre hex string formatı (%0*llx)
        char extranonce_2_str[extranonce_2_len * 2 + 1]; 
        snprintf(extranonce_2_str, sizeof(extranonce_2_str), "%0*llx", (int) extranonce_2_len * 2, (unsigned long long) extranonce_2);

        // generate coinbase tx
        int coinbase_tx_len = strlen(current_job->coinbase_1) + strlen(extranonce_str) + strlen(extranonce_2_str) +
                              strlen(current_job->coinbase_2);
        char *coinbase_tx = (char *) MALLOC(coinbase_tx_len + 1);
        snprintf(coinbase_tx, coinbase_tx_len + 1, "%s%s%s%s", current_job->coinbase_1, extranonce_str,
                 extranonce_2_str, current_job->coinbase_2);

        // calculate merkle root
        char merkle_root[65];
        calculate_merkle_root_hash(coinbase_tx, current_job->_merkle_branches, current_job->n_merkle_branches,
                                   merkle_root);
        free(coinbase_tx);

        bm_job *next_job = (bm_job *) MALLOC(sizeof(bm_job));
        construct_bm_job(current_job, merkle_root, version_mask, next_job);
        next_job->jobid = strdup(current_job->job_id);
        next_job->extranonce2 = strdup(extranonce_2_str);
        next_job->pool_diff = active_stratum_difficulty;
        next_job->pool_id = pool_id;
        next_job->asic_diff = asic_diff;

        return next_job;
    }

    bool isValid() const override
    {
        return current_job->ntime != 0 && extranonce_str && current_job->job_id && current_job->coinbase_1 &&
               current_job->coinbase_2;
    }

    bool isNewWork(uint32_t &last_ntime) const override
    {
        if (last_ntime != current_job->ntime) {
            last_ntime = current_job->ntime;
            return true;
        }
        return false;
    }

    const char* getJobId() const override
    {
        return current_job->job_id;
    }

    uint32_t getActiveDifficulty() const override
    {
        return active_stratum_difficulty;
    }

    uint32_t getVersionMask() const override
    {
        return version_mask;
    }

    void invalidate() override
    {
        current_job->ntime = 0;
        safe_free(extranonce_str);
        safe_free(next_extranonce_str);
        safe_free(current_job->job_id);
        safe_free(current_job->coinbase_1);
        safe_free(current_job->coinbase_2);
    }

    // --- V1-specific methods ---

    void set_version_mask(uint32_t mask)
    {
        version_mask = mask;
    }

    bool set_difficulty(uint32_t difficulty)
    {
        bool is_new = stratum_difficulty != difficulty;
        stratum_difficulty = difficulty;
        return is_new;
    }

    void set_enonce(char *enonce, int enonce2_len)
    {
        safe_free(extranonce_str);
        extranonce_str = strdup(enonce);
        extranonce_2_len = enonce2_len;
    }

    void set_next_enonce(char *enonce, int enonce2_len)
    {
        safe_free(next_extranonce_str);
        next_extranonce_str = strdup(enonce);
        next_extranonce_2_len = enonce2_len;
    }

    void create_job_mining_notify(mining_notify *notify)
    {
        if (next_extranonce_str) {
            safe_free(extranonce_str);
            extranonce_str = strdup(next_extranonce_str);
            extranonce_2_len = next_extranonce_2_len;
            safe_free(next_extranonce_str);
            next_extranonce_2_len = 0;
        }

        safe_free(current_job->job_id);
        safe_free(current_job->coinbase_1);
        safe_free(current_job->coinbase_2);

        memcpy(current_job, notify, sizeof(mining_notify));
        current_job->job_id = strdup(notify->job_id);
        current_job->coinbase_1 = strdup(notify->coinbase_1);
        current_job->coinbase_2 = strdup(notify->coinbase_2);

        active_stratum_difficulty = stratum_difficulty;
    }
};

// Global mining info instances - one per pool slot
static MiningInfoV1 s_miningInfoV1[2] = {MiningInfoV1{}, MiningInfoV1{}};
MiningInfoBase* miningInfo[2] = {&s_miningInfoV1[0], &s_miningInfoV1[1]};

#define min(a, b) ((a < b) ? (a) : (b))
#define max(a, b) ((a > b) ? (a) : (b))

static void create_job_timer(TimerHandle_t xTimer)
{
    pthread_mutex_lock(&job_mutex);
    pthread_cond_signal(&job_cond);
    pthread_mutex_unlock(&job_mutex);
}

void trigger_job_creation()
{
    pthread_mutex_lock(&job_mutex);
    pthread_cond_signal(&job_cond);
    pthread_mutex_unlock(&job_mutex);
}

static MiningInfoV1* ensureV1(int pool)
{
    if (miningInfo[pool] != &s_miningInfoV1[pool]) {
        s_miningInfoV1[pool].invalidate();
        miningInfo[pool] = &s_miningInfoV1[pool];
    }
    return &s_miningInfoV1[pool];
}

void create_job_set_version_mask(int pool, uint32_t mask)
{
    PThreadGuard g(current_stratum_job_mutex);
    ensureV1(pool)->set_version_mask(mask);
}

bool create_job_set_difficulty(int pool, uint32_t difficulty)
{
    PThreadGuard g(current_stratum_job_mutex);
    return ensureV1(pool)->set_difficulty(difficulty);
}

void create_job_set_enonce(int pool, char *enonce, int enonce2_len)
{
    PThreadGuard g(current_stratum_job_mutex);
    ensureV1(pool)->set_enonce(enonce, enonce2_len);
}

void set_next_enonce(int pool, char *enonce, int enonce2_len)
{
    PThreadGuard g(current_stratum_job_mutex);
    ensureV1(pool)->set_next_enonce(enonce, enonce2_len);
}

void create_job_mining_notify(int pool, mining_notify *notify, bool abandonWork)
{
    {
        PThreadGuard g(current_stratum_job_mutex);
        if (abandonWork) {
            asicJobs.cleanJobs(pool);
        }
        ensureV1(pool)->create_job_mining_notify(notify);
    }
    trigger_job_creation();
}

void create_job_invalidate(int pool)
{
    PThreadGuard g(current_stratum_job_mutex);
    miningInfo[pool]->invalidate();
    asicJobs.cleanJobs(pool);
}

void create_jobs_task(void *pvParameters)
{
    Board *board = SYSTEM_MODULE.getBoard();
    Asic *asics = board->getAsics();

    ESP_LOGI(TAG, "ASIC Job Interval: %d ms", board->getAsicJobIntervalMs());
    SYSTEM_MODULE.notifyMiningStarted();
    ESP_LOGI(TAG, "ASIC Ready!");

    TimerHandle_t job_timer = xTimerCreate(TAG, pdMS_TO_TICKS(board->getAsicJobIntervalMs()), pdTRUE, NULL, create_job_timer);

    if (job_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create timer");
        return;
    }

    if (xTimerStart(job_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start timer");
        return;
    }

    uint32_t last_ntime[2]{0};
    uint64_t last_submit_time = 0;

    int lastJobInterval = board->getAsicJobIntervalMs();

    while (1) {
        if (POWER_MANAGEMENT_MODULE.isShutdown()) {
            ESP_LOGW(TAG, "suspended");
            vTaskSuspend(NULL);
        }
        pthread_mutex_lock(&job_mutex);
        pthread_cond_wait(&job_cond, &job_mutex);
        pthread_mutex_unlock(&job_mutex);

        if (board->getAsicJobIntervalMs() != lastJobInterval) {
            xTimerChangePeriod(job_timer, pdMS_TO_TICKS(board->getAsicJobIntervalMs()), 0);
            lastJobInterval = board->getAsicJobIntervalMs();
            continue;
        }

        bm_job *next_job = nullptr;
        int active_pool = 0;
        const char *active_pool_str = "";

        if (!STRATUM_MANAGER) {
            continue;
        }

        active_pool = STRATUM_MANAGER->getNextActivePool();
        active_pool_str = active_pool ? "Sec" : "Pri";

        uint64_t extranonce_2 = 0;

        { 
            PThreadGuard g(current_stratum_job_mutex);
            MiningInfoBase *mi = miningInfo[active_pool];

            if (!mi->isValid() || !asics) {
                continue;
            }

            MiningInfoV1 *v1_info = (MiningInfoV1*)mi;

            // --- OTOMATİK 4 VEYA 8 BAYT UNIFORM RANDOM SEÇİMİ ---
            if (v1_info->extranonce_2_len > 4) {
                // 8 Bayt (64-bit): 0 ile 0xFFFFFFFFFFFFFFFF arasında tam rastgele
                extranonce_2 = ((uint64_t)esp_random() << 32) | esp_random();
            } else {
                // 4 Bayt (32-bit): 0 ile 0xFFFFFFFF arasında tam rastgele
                extranonce_2 = (uint64_t)esp_random();
            }

            if (mi->isNewWork(last_ntime[active_pool])) {
                ESP_LOGI(TAG, "(%s) New Work Received %s", active_pool_str, mi->getJobId());
            }

            uint32_t asic_diff = STRATUM_MANAGER->selectAsicDiff(active_pool, mi->getActiveDifficulty());
            next_job = mi->buildBmJob(extranonce_2, active_pool, asic_diff);
        }

        asics->setJobDifficultyMask(next_job->asic_diff);

        uint64_t current_time = esp_timer_get_time();
        if (last_submit_time) {
            ESP_LOGD(TAG, "(%s) job interval %dms", active_pool_str, (int) ((current_time - last_submit_time) / 1e3));
        }
        last_submit_time = current_time;

        int asic_job_id = asics->sendWork((uint32_t)extranonce_2, next_job);

        ESP_LOGD(TAG, "(%s) Sent Job (%d): %02X", active_pool_str, active_pool, asic_job_id);

        asicJobs.storeJob(next_job, asic_job_id);

        // --- CAN: send raw job to each slave ---
        for (uint8_t slave = 0; slave < CAN_SLAVE_MAX; slave++) {
            if (!can_master_is_slave_active(slave)) continue;

            bm_job *slave_job = nullptr;
            uint64_t slave_enonce = 0;

            {
                PThreadGuard g(current_stratum_job_mutex);
                MiningInfoBase *mi = miningInfo[active_pool];
                if (mi->isValid()) {
                    MiningInfoV1 *v1_info = (MiningInfoV1*)mi;
                    
                    if (v1_info->extranonce_2_len > 4) {
                        slave_enonce = ((uint64_t)esp_random() << 32) | esp_random();
                    } else {
                        slave_enonce = (uint64_t)esp_random();
                    }

                    uint32_t asic_diff = STRATUM_MANAGER->selectAsicDiff(active_pool, mi->getActiveDifficulty());
                    slave_job = mi->buildBmJob(slave_enonce, active_pool, asic_diff);
                }
            }

            if (slave_job) {
                can_send_raw_job(slave, (uint8_t) asic_job_id, slave_job);
                slaveAsicJobs[slave].storeJob(slave_job, asic_job_id);
            }
        }
    }
}
