#include <ultra64.h>

#include "sm64.h"
#include "main.h"
#include "memory.h"
#include "save_file.h"
#include "seq_ids.h"
#include "sound_init.h"
#include "display.h"
#include "engine/level_script.h"
#include "profiler.h"
#include "print.h"
#include "segment2.h"
#include "main_entry.h"
#include "audio/external.h"
#include <prevent_bss_reordering.h>
#include "game.h"
#include "../../enhancements/crash.inc.c"

// FIXME: I'm not sure all of these variables belong in this file, but I don't
// know of a good way to split them
struct Controller gControllers[3];
OSContStatus gControllerStatuses[4];
OSContPad gControllerPads[4];
OSMesgQueue gGameVblankQueue;
OSMesgQueue D_80339CB8;
OSMesg D_80339CD0;
OSMesg D_80339CD4;
struct VblankHandler gGameVblankHandler;
u32 gFrameBuffers[3];
u32 zBufferPtr;
void *D_80339CF0;
void *D_80339CF4;
void *D_80339CF0Luigi;
void *D_80339CF4Luigi;
struct SPTask *gGfxSPTask;
Gfx *gDisplayListHead;
u8 *gGfxPoolEnd;
struct GfxPool *gGfxPool;
u8 gControllerBits;
s8 gEepromProbe;

struct MarioAnimation D_80339D10[2];
struct MarioAnimation gDemo;
UNUSED s8 filler80339D30[0x80339DC0 - 0x80339D30];

void (*D_8032C6A0)(void) = NULL;
struct Controller *gPlayer1Controller = &gControllers[0];
struct Controller *gPlayer2Controller = &gControllers[1];
// probably debug only, see note below
struct Controller *gPlayer3Controller = &gControllers[2];
struct DemoInput *gCurrDemoInput = NULL; // demo input sequence
u16 gDemoInputListID = 0;
const u8 activePlayers = 2;
struct DemoInput gRecordedDemoInput = { 0 }; // possibly removed in EU. TODO: Check

// this function records distinct inputs over a 255-frame interval to RAM locations and was likely
// used to record the demo sequences seen in the final game. This function is unused.
static void record_demo(void) {
}

// take the updated controller struct and calculate
// the new x, y, and distance floats.
void adjust_analog_stick(struct Controller *controller) {
    UNUSED u8 pad[8];

    // reset the controller's x and y floats.
    controller->stickX = 0;
    controller->stickY = 0;

    // modulate the rawStickX and rawStickY to be the new f32 values by adding/subtracting 6.
    if (controller->rawStickX <= -8) {
        controller->stickX = controller->rawStickX + 6;
    }

    if (controller->rawStickX >= 8) {
        controller->stickX = controller->rawStickX - 6;
    }

    if (controller->rawStickY <= -8) {
        controller->stickY = controller->rawStickY + 6;
    }

    if (controller->rawStickY >= 8) {
        controller->stickY = controller->rawStickY - 6;
    }

    // calculate f32 magnitude from the center by vector length.
    controller->stickMag =
        sqrtf(controller->stickX * controller->stickX + controller->stickY * controller->stickY);

    // magnitude cannot exceed 64.0f: if it does, modify the values appropriately to
    // flatten the values down to the allowed maximum value.
    if (controller->stickMag > 64) {
        controller->stickX *= 64 / controller->stickMag;
        controller->stickY *= 64 / controller->stickMag;
        controller->stickMag = 64;
    }
}

// if a demo sequence exists, this will run the demo
// input list until it is complete. called every frame.
void run_demo_inputs(void) {
    // eliminate the unused bits.
    gControllers[0].controllerData->button &= VALID_BUTTONS;

    /*
        Check if a demo inputs list
        exists and if so, run the
        active demo input list.
    */
    if (gCurrDemoInput != NULL) {
        /*
            clear player 2's inputs if they exist. Player 2's controller
            cannot be used to influence a demo. At some point, Nintendo
            may have planned for there to be a demo where 2 players moved
            around instead of just one, so clearing player 2's influence from
            the demo had to have been necessary to perform this. Co-op mode, perhaps?
        */

        // the timer variable being 0 at the current input means the demo is over.
        // set the button to the END_DEMO mask to end the demo.
        if (gCurrDemoInput->timer == 0) {
            gControllers[0].controllerData->stick_x = 0;
            gControllers[0].controllerData->stick_y = 0;
            gControllers[0].controllerData->button = END_DEMO;
        } else {
            // backup the start button if it is pressed, since we don't want the
            // demo input to override the mask where start may have been pressed.
            u16 startPushed =
                (gControllers[1].controllerData->button | gControllers[0].controllerData->button)
                & START_BUTTON;

            // perform the demo inputs by assigning the current button mask and the stick inputs.
            gControllers[0].controllerData->stick_x = gCurrDemoInput->rawStickX;
            gControllers[0].controllerData->stick_y = gCurrDemoInput->rawStickY;

            /*
                to assign the demo input, the button information is stored in
                an 8-bit mask rather than a 16-bit mask. this is because only
                A, B, Z, Start, and the C-Buttons are used in a demo, as bits
                in that order. In order to assign the mask, we need to take the
                upper 4 bits (A, B, Z, and Start) and shift then left by 8 to
                match the correct input mask. We then add this to the masked
                lower 4 bits to get the correct button mask.
            */
            gControllers[0].controllerData->button =
                ((gCurrDemoInput->button & 0xF0) << 8) + ((gCurrDemoInput->button & 0xF));

            // if start was pushed, put it into the demo sequence being input to
            // end the demo.
            gControllers[0].controllerData->button |= startPushed;

            // run the current demo input's timer down. if it hits 0, advance the
            // demo input list.
            if (--gCurrDemoInput->timer == 0) {
                gCurrDemoInput++;
            }
        }
    }
}

// update the controller struct with available inputs if present.
void read_controller_inputs(void) {
    s32 i;

    // if any controllers are plugged in, update the
    // controller information.

    for (i = 0; i < activePlayers; i++) {
        struct Controller *controller = &gControllers[i];

        // if we're receiving inputs, update the controller struct
        // with the new button info.
        if (controller->controllerData != NULL) {
            controller->rawStickX = controller->controllerData->stick_x;
            controller->rawStickY = controller->controllerData->stick_y;
            controller->buttonPressed = controller->controllerData->button
                                        & (controller->controllerData->button ^ controller->buttonDown);
            // 0.5x A presses are a good meme
            controller->buttonDown = controller->controllerData->button;
            adjust_analog_stick(controller);
        } else // otherwise, if the controllerData is NULL, 0 out all of the inputs.
        {
            controller->rawStickX = 0;
            controller->rawStickY = 0;
            controller->buttonPressed = 0;
            controller->buttonDown = 0;
            controller->stickX = 0;
            controller->stickY = 0;
            controller->stickMag = 0;
        }
    }

    // For some reason, player 1's inputs are copied to player 3's port. This
    // potentially may have been a way the developers "recorded" the inputs
    // for demos, despite record_demo existing.
    gPlayer3Controller->rawStickX = gPlayer1Controller->rawStickX;
    gPlayer3Controller->rawStickY = gPlayer1Controller->rawStickY;
    gPlayer3Controller->stickX = gPlayer1Controller->stickX;
    gPlayer3Controller->stickY = gPlayer1Controller->stickY;
    gPlayer3Controller->stickMag = gPlayer1Controller->stickMag;
    gPlayer3Controller->buttonPressed = gPlayer1Controller->buttonPressed;
    gPlayer3Controller->buttonDown = gPlayer1Controller->buttonDown;
}

// initialize the controller structs to point at the OSCont information.
void init_controllers(void) {
    s16 port, cont;

    // set controller 1 to point to the set of status/pads for input 1 and
    // init the controllers.
    gControllers[0].statusData = &gControllerStatuses[0];
    gControllers[0].controllerData = &gControllerPads[0];
    osContInit(&gSIEventMesgQueue, &gControllerBits, &gControllerStatuses[0]);
    // strangely enough, the EEPROM probe for save data is done in this function.
    // save pak detection?
    gEepromProbe = osEepromProbe(&gSIEventMesgQueue);

    // loop over the 4 ports and link the controller structs to the appropriate
    // status and pad. Interestingly, although there are pointers to 3 controllers,
    // only 2 are connected here. The third seems to have been reserved for debug
    // purposes and was never connected in the retail ROM, thus gPlayer3Controller
    // cannot be used, despite being referenced in various code.
    for (cont = 0, port = 0; port < 4 && cont < 2; port++) {
        // is controller plugged in?
        if (gControllerBits & (1 << port)) {
            // the game allows you to have just 1 controller plugged
            // into any port in order to play the game. this was probably
            // so if any of the ports didnt work, you can have controllers
            // plugged into any of them and it will work.
            gControllers[cont].statusData = &gControllerStatuses[port];
            gControllers[cont++].controllerData = &gControllerPads[port];
        }
    }
}

void setup_game_memory(void) {
    UNUSED u8 pad[8];

    set_segment_base_addr(0, (void *) 0x80000000);
    osCreateMesgQueue(&D_80339CB8, &D_80339CD4, 1);
    osCreateMesgQueue(&gGameVblankQueue, &D_80339CD0, 1);
    zBufferPtr = VIRTUAL_TO_PHYSICAL(gZBuffer);
    gFrameBuffers[0] = VIRTUAL_TO_PHYSICAL(gFrameBuffer0);
    gFrameBuffers[1] = VIRTUAL_TO_PHYSICAL(gFrameBuffer1);
    gFrameBuffers[2] = VIRTUAL_TO_PHYSICAL(gFrameBuffer2);
    D_80339CF0 = main_pool_alloc(0x4000, MEMORY_POOL_LEFT);
    set_segment_base_addr(17, (void *) D_80339CF0);
    func_80278A78(&D_80339D10[0], gMarioAnims, D_80339CF0);
    D_80339CF0Luigi = main_pool_alloc(0x4000, MEMORY_POOL_LEFT);
    set_segment_base_addr(17, (void *) D_80339CF0Luigi);
    func_80278A78(&D_80339D10[1], gMarioAnims, D_80339CF0Luigi);
    D_80339CF4 = main_pool_alloc(2048, MEMORY_POOL_LEFT);
    set_segment_base_addr(24, (void *) D_80339CF4);
    func_80278A78(&gDemo, gDemoInputs, D_80339CF4);
    load_segment(0x10, _entrySegmentRomStart, _entrySegmentRomEnd, MEMORY_POOL_LEFT);
    load_segment_decompress(2, _segment2_mio0SegmentRomStart, _segment2_mio0SegmentRomEnd);
}

// main game loop thread. runs forever as long as the game
// continues.
void thread5_game_loop(UNUSED void *arg) {
    struct LevelCommand *addr;

    setup_game_memory();
    init_controllers();
    save_file_load_all();

    set_vblank_handler(2, &gGameVblankHandler, &gGameVblankQueue, (OSMesg) 1);

    // point addr to the entry point into the level script data.
    addr = segmented_to_virtual(level_script_entry);

    play_music(2, SEQUENCE_ARGS(0, SEQ_SOUND_PLAYER), 0);
    set_sound_mode(save_file_get_sound_mode());
    func_80247ED8();

    while (1) {
        // if the reset timer is active, run the process to reset the game.
        if (gResetTimer) {
            func_80247D84();
           // gCurrLevelNum = LEVEL_MIN;
           // luigiCamFirst = 0;
            continue;
        }
        profiler_log_thread5_time(THREAD5_START);

        // if any controllers are plugged in, start read the data for when
        // read_controller_inputs is called later.
        if (gControllerBits) {
            osContStartReadData(&gSIEventMesgQueue);
        }

        audio_game_loop_tick();
        func_80247FAC();
        if (gControllerBits) {
            osRecvMesg(&gSIEventMesgQueue, &D_80339BEC, OS_MESG_BLOCK);
            osContGetReadData(&gControllerPads[0]);
        }
        if (!luigiCamFirst) { // dont drop inputs 50% of the time. you also need to put the game to
                              // 60fps and run object loops every other frame. implement frame skip.
            read_controller_inputs();
        }
        addr = level_script_execute(addr);

        display_and_vsync();

        // when debug info is enabled, print the "BUF %d" information.
        if (FALSE) {
            // subtract the end of the gfx pool with the display list to obtain the
            // amount of free space remaining.
            print_text_fmt_int(180, 20, "BUF %d", gGfxPoolEnd - (u8 *) gDisplayListHead);
        }
    }
}
