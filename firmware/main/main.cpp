// main.cpp - r-esp32-evil firmware entry.
//
// app_main brings the board up and starts the game loop as its own task on
// the game core with an internal-RAM stack; the drivers (panel DMA, I2S,
// audio stream pump, browser link) run on the other core.
#include "board.h"
#include "re1_game.h"
#include "re1_target.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "main";

static void GameTask(void*)
{
    re1_game_run();
    ESP_LOGW(TAG, "game exited; restarting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

extern "C" void app_main(void)
{
    board_init();
    if (!board_storage_init()) {
        ESP_LOGE(TAG, "no game data - see docs/GUIDE.md, 'Preparing the SD card'");
        return;
    }
    ESP_LOGI(TAG, "starting game: PSRAM free %u KB, internal free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    xTaskCreatePinnedToCore(GameTask, "re1_game", CONFIG_RE1_GAME_STACK_KB * 1024, nullptr, 5,
                            nullptr, CONFIG_RE1_GAME_CORE);
}
