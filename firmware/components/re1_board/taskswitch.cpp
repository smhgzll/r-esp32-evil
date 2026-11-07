// taskswitch.cpp - the game's cooperative scheduler on FreeRTOS.
//
// decomp/src/game/TaskScheduler.cpp (RE1_PORT_TASKSWITCH_EXTERNAL) needs three
// primitives with the semantics of the PC's stack switch: start a task body on
// a slot, resume it where it yielded, yield back to the scheduler. Xtensa's
// windowed-register ABI makes a hand-written stack switch fragile, so each slot
// is a real FreeRTOS task and control is handed back and forth with direct
// task notifications: exactly one of {scheduler, slot tasks} runs at any time,
// all on the game core, which is the original's single-threaded behaviour.
//
// A slot that is started again (Task_chain, or a dead slot re-executed) gets a
// fresh FreeRTOS task; the old one, blocked in its yield, is deleted - the
// equivalent of makecontext discarding the old ucontext. Stacks live in PSRAM.
#include "board.h"
#include "re1_target.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define SLOTS 3

static TaskHandle_t s_scheduler = nullptr;
static TaskHandle_t s_slot[SLOTS] = {nullptr, nullptr, nullptr};
static void (*s_entry[SLOTS])(void);

static void SlotMain(void* arg)
{
    const int id = (int)(intptr_t)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for the first switch-in
    s_entry[id]();
    // TaskEntry ends with Task_exit, which yields and is never resumed; if a
    // body ever returned here, park the task the same way.
    for (;;) re1port_task_yield(id);
}

extern "C" void re1port_task_start(int id, void (*entry)(void))
{
    if (id < 0 || id >= SLOTS) return;
    s_scheduler = xTaskGetCurrentTaskHandle();
    if (s_slot[id] != nullptr) {
        vTaskDeleteWithCaps(s_slot[id]);
        s_slot[id] = nullptr;
    }
    s_entry[id] = entry;
    UBaseType_t prio = uxTaskPriorityGet(nullptr);
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        SlotMain, "re1_task", CONFIG_RE1_TASK_STACK_KB * 1024, (void*)(intptr_t)id, prio,
        &s_slot[id], CONFIG_RE1_GAME_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) re1t_fatal("cannot create a game task (PSRAM exhausted?)");
    xTaskNotifyGive(s_slot[id]);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // until the task yields
}

extern "C" void re1port_task_resume(int id)
{
    if (id < 0 || id >= SLOTS || s_slot[id] == nullptr) return;
    s_scheduler = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(s_slot[id]);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

extern "C" void re1port_task_yield(int id)
{
    (void)id;
    xTaskNotifyGive(s_scheduler);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// The board never uses the slab from plat_alloc_guarded_stacks (each slot task
// allocates its own stack); TaskScheduler_Init only needs a non-NULL base.
void* plat_alloc_guarded_stacks(int count, size_t stackSize, size_t guardSize)
{
    (void)count; (void)stackSize; (void)guardSize;
    static unsigned char s_dummy[16];
    return s_dummy;
}
