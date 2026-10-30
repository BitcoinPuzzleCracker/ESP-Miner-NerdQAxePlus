#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "create_jobs_task";

// Stratum & Job State Structuur
typedef struct {
    char job_id[64];
    uint32_t version;
    uint32_t version_mask;
    bool clean_jobs;
    bool is_valid;
} stratum_job_t;

static stratum_job_t current_job = {0};
static portMUX_TYPE job_spinlock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Wordt aangeroepen wanneer er een nieuwe mining.notify van de pool binnenkomt
 */
void on_stratum_notify(const char *new_job_id, uint32_t base_version, uint32_t mask, bool clean) {
    taskENTER_CRITICAL(&job_spinlock);

    // Werk de actieve Job ID en parameters bij
    strncpy(current_job.job_id, new_job_id, sizeof(current_job.job_id) - 1);
    current_job.version = base_version;
    current_job.version_mask = mask;
    current_job.clean_jobs = clean;
    current_job.is_valid = true;

    taskEXIT_CRITICAL(&job_spinlock);

    if (clean) {
        ESP_LOGI(TAG, "Nieuwe schone job ontvangen [%s]. Reset interne werk-pipeline.", new_job_id);
        // Optioneel: Flush de ASIC werk-queue hier als clean == true
    } else {
        ESP_LOGD(TAG, "Job update ontvangen [%s] (clean=false). Synchroniseer Job ID, blijf tollen.", new_job_id);
    }
}

/**
 * @brief Hoofdtask die werkpakketten (header + rolled version) genereert voor de ASIC
 */
void create_jobs_task(void *pvParameters) {
    uint32_t version_rolling_counter = 0;
    stratum_job_t local_job;

    while (1) {
        // Kopieer de actieve job-status veilig vanuit de kritieke sectie
        taskENTER_CRITICAL(&job_spinlock);
        memcpy(&local_job, &current_job, sizeof(stratum_job_t));
        taskEXIT_CRITICAL(&job_spinlock);

        if (!local_job.is_valid) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // --- VERSION ROLLING LOGICA ---
        // Pas de gerolde bits toe binnen het toegestane masker (bijv. 0x1FFFE000)
        uint32_t rolled_version = (local_job.version & ~local_job.version_mask) | 
                                 ((version_rolling_counter << 13) & local_job.version_mask);

        // Verhoog de teller continu (blijf tollen)
        version_rolling_counter++;

        // --- STUUR WERK NAAR ASIC QUEUE ---
        // Zorg dat de ASIC-job GEKOPPELD blijft aan de meest actuele local_job.job_id!
        // submit_to_asic_pipeline(local_job.job_id, rolled_version, ...);

        vTaskDelay(pdMS_TO_TICKS(1)); // Geef FreeRTOS ademruimte
    }
}

/**
 * @brief Afhandeling van de ASIC resultaten (shares) vóór verzending naar de pool
 */
void on_asic_result_found(const char *job_id_from_share, uint32_t found_nonce, uint32_t rolled_version) {
    char active_id[64];

    taskENTER_CRITICAL(&job_spinlock);
    strncpy(active_id, current_job.job_id, sizeof(active_id) - 1);
    taskEXIT_CRITICAL(&job_spinlock);

    // DROP-CHECK: Controleer of de gevonden share nog wel bij de HUIDIGE actieve job hoort
    if (strcmp(job_id_from_share, active_id) != 0) {
        ESP_LOGW(TAG, "Share genegeerd (verouderde job: %s, actief: %s). Voorkomt Error 21!", 
                 job_id_from_share, active_id);
        return; // Verstuur NIET naar de pool om "Job not found" (rejected) te voorkomen
    }

    // Als de Job ID exact overeenkomt, stuur veilig de mining.submit
    // stratum_send_submit(active_id, found_nonce, rolled_version);
    ESP_LOGI(TAG, "Geldige share verzonden! Job: %s, Ver: %08X", active_id, rolled_version);
}
