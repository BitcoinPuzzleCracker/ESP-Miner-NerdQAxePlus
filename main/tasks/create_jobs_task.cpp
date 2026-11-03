#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
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
// INLINE EXTRANONCE2 GENERATOR — cyclisch 1 t/m 16 hex-karakters
// ============================================================================

#define EXTRANONCE2_MAX_BYTES 8
#define EXTRANONCE2_NUM_POOLS 2
#define E2_JOBS_PER_PHASE     20
#define E2_HEX_CHARS_MIN      1
#define E2_HEX_CHARS_MAX      16

#ifndef CAN_SLAVE_MAX
#define CAN_SLAVE_MAX 8
#endif

// Testvectoren — worden 1x afgelopen bij de start
static const uint64_t E2_TEST_VECTORS[] = {
    0x0000000000000001ULL,
    0x0000000000000002ULL,
    0x0000000000000003ULL,
    0x0000000000000004ULL,
    0x0000000000000010ULL,
    0x0000000000000020ULL,
    0x0000000000000022ULL,
    0x0000000000000064ULL,
    0x00000000000000FFULL,
    0x0000000000000100ULL,
    0x0000000000000FFFULL,
    0x000000000000FFFFULL,
    0x0000000000010000ULL,
    0x00000000FFFFFFFFULL,
    0x0000000100000000ULL,
    0x0000000100000001ULL,
    0x0123456789ABCDEFULL,
    0x7FFFFFFFFFFFFFFFULL,
    0x8000000000000000ULL,
    0xFFFFFFFFFFFFFF00ULL,
    0xFFFFFFFFFFFFFFFEULL,
    0xFFFFFFFFFFFFFFFFULL,
};
#define E2_NUM_TEST_VECTORS (sizeof(E2_TEST_VECTORS) / sizeof(E2_TEST_VECTORS[0]))

typedef struct {
    uint64_t counter;
    uint64_t seed;
    size_t   tv_idx;
    int      phase_hex_chars;   // 1 .. 16
    uint32_t phase_remaining;
    bool     in_test_vectors;
    bool     initialized;
} e2_state_t;

static e2_state_t      s_e2_pool[EXTRANONCE2_NUM_POOLS];
static e2_state_t      s_e2_slave[CAN_SLAVE_MAX];
static pthread_mutex_t s_e2_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t e2_splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline uint64_t e2_random_u64(void)
{
    return ((uint64_t) esp_random() << 32) | esp_random();
}

static uint64_t e2_state_next(e2_state_t *st, int len_bytes, uint64_t high_bits, const char *name)
{
    if (len_bytes <= 0) len_bytes = EXTRANONCE2_MAX_BYTES;
    if (len_bytes > EXTRANONCE2_MAX_BYTES) len_bytes = EXTRANONCE2_MAX_BYTES;

    int total_bits = len_bytes * 8;
    int max_hex_chars = len_bytes * 2;   // 8 bytes => 16 hex chars
    uint64_t total_mask = (total_bits >= 64) ? ~0ULL : ((1ULL << total_bits) - 1);

    if (!st->initialized) {
        st->seed             = e2_random_u64();
        st->counter          = st->seed;
        st->tv_idx           = 0;
        st->in_test_vectors  = true;
        st->phase_hex_chars  = E2_HEX_CHARS_MIN;
        st->phase_remaining  = E2_JOBS_PER_PHASE;
        st->initialized      = true;
        ESP_LOGI(TAG, "[%s] e2 seed=%016llx len=%d start",
                 name, (unsigned long long) st->seed, len_bytes);
    }

    uint64_t v;

    // --- TESTVECTOREN EERST ---
    if (st->in_test_vectors) {
        v = E2_TEST_VECTORS[st->tv_idx++];
        if (st->tv_idx >= E2_NUM_TEST_VECTORS) {
            st->in_test_vectors  = false;
            st->phase_hex_chars  = E2_HEX_CHARS_MIN;
            st->phase_remaining  = E2_JOBS_PER_PHASE;
            ESP_LOGI(TAG, "[%s] e2 phase -> %d hex chars (x%u)",
                     name, st->phase_hex_chars, E2_JOBS_PER_PHASE);
        }
    } else {
        // Bepaal hoeveel bits deze fase gebruikt (hex_chars * 4)
        int phase_hex = st->phase_hex_chars;
        if (phase_hex > max_hex_chars) phase_hex = max_hex_chars;
        int phase_bits = phase_hex * 4;
        if (phase_bits > 64) phase_bits = 64;

        v = e2_splitmix64(st->counter++);

        if (phase_bits < 64) {
            v &= ((1ULL << phase_bits) - 1);
        }

        // Volgende fase na N jobs
        if (--st->phase_remaining == 0) {
            st->phase_remaining = E2_JOBS_PER_PHASE;
            if (st->phase_hex_chars >= max_hex_chars) {
                st->phase_hex_chars = E2_HEX_CHARS_MIN;   // ← loop terug
            } else {
                st->phase_hex_chars++;
            }
            ESP_LOGI(TAG, "[%s] e2 phase -> %d hex chars (x%u)",
                     name, st->phase_hex_chars, E2_JOBS_PER_PHASE);
        }
    }

    v &= total_mask;
    v = (v & ~high_bits) | (high_bits & total_mask);
    return v;
}

static uint64_t e2_next_pool(int pool, int len_bytes)
{
    if (pool < 0 || pool >= EXTRANONCE2_NUM_POOLS) pool = 0;
    pthread_mutex_lock(&s_e2_lock);
    uint64_t v = e2_state_next(&s_e2_pool[pool], len_bytes, 0,
                               pool ? "Sec" : "Pri");
    pthread_mutex_unlock(&s_e2_lock);
    return v;
}

static uint64_t e2_next_slave(uint8_t slave_id, int len_bytes)
{
    if (slave_id >= CAN_SLAVE_MAX) return 0;
    uint64_t high_bits = ((uint64_t) slave_id) << 56;
    char name[8];
    snprintf(name, sizeof(name), "S%u", slave_id);
    pthread_mutex_lock(&s_e2_lock);
    uint64_t v = e2_state_next(&s_e2_slave[slave_id], len_bytes, high_bits, name);
    pthread_mutex_unlock(&s_e2_lock);
    return v;
}

// ============================================================================
// MiningInfoBase
// ============================================================================

MiningInfoBase::~MiningInfoBase() {}

// ============================================================================
// MiningInfoV1
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

    bm_job* buildBmJob(uint64_t extranonce_2, int pool_id, uint32_t asic_diff) override
    {
        char extranonce_2_str[extranonce_2_len * 2 + 1];
        snprintf(extranonce_2_str, sizeof(extranonce_2_str),
                 "%0*llx", (int) extranonce_2_len * 2,
                 (unsigned long long) extranonce_2);

        int coinbase_tx_len = strlen(current_job->coinbase_1) + strlen(extranonce_str) +
                              strlen(extranonce_2_str) + strlen(current_job->coinbase_2);
        char *coinbase_tx = (char *) MALLOC(coinbase_tx_len + 1);
        snprintf(coinbase_tx, coinbase_tx_len + 1, "%s%s%s%s",
                 current_job->coinbase_1, extranonce_str,
                 extranonce_2_str, current_job->coinbase_2);

        char merkle_root[65];
        calculate_merkle_root_hash(coinbase_tx, current_job->_merkle_branches,
                                   current_job->n_merkle_branches, merkle_root);
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
        return current_job->ntime != 0 && extranonce_str && current_job->job_id &&
               current_job->coinbase_1 && current_job->coinbase_2;
    }

    bool isNewWork(uint32_t &last_ntime) const override
    {
        if (last_ntime != current_job->ntime) {
            last_ntime = current_job->ntime;
            return true;
        }
        return false;
    }

    const char* getJobId() const override { return current_job->job_id; }
    uint32_t getActiveDifficulty() const override { return active_stratum_difficulty; }
    uint32_t getVersionMask() const override { return version_mask; }

    void invalidate() override
    {
        current_job->ntime = 0;
        safe_free(extranonce_str);
        safe_free(next_extranonce_str);
        safe_free(current_job->job_id);
        safe_free(current_job->coinbase_1);
        safe_free(current_job->coinbase_2);
    }

    void set_version_mask(uint32_t mask) { version_mask = mask; }

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
        current_job->job_id    = strdup(notify->job_id);
        current_job->coinbase_1 = strdup(notify->coinbase_1);
        current_job->coinbase_2 = strdup(notify->coinbase_2);

        active_stratum_difficulty = stratum_difficulty;
    }
};

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

    TimerHandle_t job_timer = xTimerCreate(TAG,
        pdMS_TO_TICKS(board->getAsicJobIntervalMs()), pdTRUE, NULL, create_job_timer);

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
            xTimerChangePeriod(job_timer,
                pdMS_TO_TICKS(board->getAsicJobIntervalMs()), 0);
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

        int e2_len;
        {
            PThreadGuard g(current_stratum_job_mutex);
            e2_len = s_miningInfoV1[active_pool].extranonce_2_len;
            if (e2_len <= 0) e2_len = EXTRANONCE2_MAX_BYTES;
        }

        { // scope for mutex
            PThreadGuard g(current_stratum_job_mutex);

            MiningInfoBase *mi = miningInfo[active_pool];

            if (!mi->isValid() || !asics) {
                continue;
            }

            if (mi->isNewWork(last_ntime[active_pool])) {
                ESP_LOGI(TAG, "(%s) New Work Received %s",
                         active_pool_str, mi->getJobId());
            }

            uint64_t e2 = e2_next_pool(active_pool, e2_len);

            uint32_t asic_diff = STRATUM_MANAGER->selectAsicDiff(
                                     active_pool, mi->getActiveDifficulty());

            next_job = mi->buildBmJob(e2, active_pool, asic_diff);

            ESP_LOGI(TAG, "(%s) e2=%0*llx",
                     active_pool_str, e2_len * 2,
                     (unsigned long long) e2);
        } // mutex

        asics->setJobDifficultyMask(next_job->asic_diff);

        uint64_t current_time = esp_timer_get_time();
        if (last_submit_time) {
            ESP_LOGD(TAG, "(%s) job interval %dms", active_pool_str,
                     (int) ((current_time - last_submit_time) / 1e3));
        }
        last_submit_time = current_time;

        int asic_job_id = asics->sendWork(0, next_job);

        ESP_LOGD(TAG, "(%s) Sent Job (%d): %02X",
                 active_pool_str, active_pool, asic_job_id);

        asicJobs.storeJob(next_job, asic_job_id);

        // --- CAN slaves ---
        for (uint8_t slave = 0; slave < CAN_SLAVE_MAX; slave++) {
            if (!can_master_is_slave_active(slave)) continue;

            uint64_t e2 = e2_next_slave(slave, e2_len);

            bm_job *slave_job = nullptr;
            {
                PThreadGuard g(current_stratum_job_mutex);
                MiningInfoBase *mi = miningInfo[active_pool];
                if (mi->isValid()) {
                    uint32_t asic_diff = STRATUM_MANAGER->selectAsicDiff(
                                             active_pool, mi->getActiveDifficulty());
                    slave_job = mi->buildBmJob(e2, active_pool, asic_diff);
                }
            }

            if (slave_job) {
                can_send_raw_job(slave, (uint8_t) asic_job_id, slave_job);
                slaveAsicJobs[slave].storeJob(slave_job, asic_job_id);
            }
        }
    }
}
