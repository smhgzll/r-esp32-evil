// Input.cpp - CMarniDirectInput for every r-esp32-evil target.
//
// Counterpart of decomp/src/platform/linux/input.cpp. The pad mask contract is
// unchanged (docs/GAMEPAD_INPUT.md in the decomp):
//   bits 0-3  axis directions (up, down, left, right)
//   bits 8-19 buttons (MARNI_XI_BTN_*)
// The mask is the OR of the board's own controls (re1t_pad_mask: analog
// stick(s) + buttons, see firmware/components/re1_board/input.c) and whatever
// the browser bridge sends (keyboard keys arrive separately as VK codes).
#include "Globals.h"
#include "platform/platform.h"
#include "marni/MarniInput.h"
#include "marni/MarniXInput.h"

#include "re1_remote.h"
#include "re1_target.h"

extern "C" int re1port_input_muted(void);   // PortMenu.cpp

static DWORD BuildPadMask(void)
{
    // The port menu owns the controls while it is up (and until they are
    // released after it closes).
    if (re1port_input_muted()) return 0;
    DWORD mask = (re1t_pad_mask() | re1_remote_pad_mask()) & ~RE1_PAD_PORT_MENU;
    // Opposite directions cannot be held at once; UP beats DOWN, LEFT beats
    // RIGHT, matching PlayerPad_Update's own post-pass.
    if (mask & 1) mask &= ~2u;
    if (mask & 4) mask &= ~8u;
    return mask;
}

static bool PadPresent(void)
{
    return re1t_pad_present() != 0 || re1_remote_pad_mask() != 0;
}

void CMarniDirectInput::UpdateKeyboardInputState(MasterInputState* pState)
{
    // 0x004202f0: zero keyboardCurr, then test all 32 mapped keys.
    pState->keyboardCurr = 0;
    for (int i = 0; i < 32; i++) {
        if (plat_key_state(pState->keyMap[i]) & 0x8000) {
            pState->keyboardCurr |= (1u << i);
        }
    }
}

void CMarniDirectInput::UpdateAllInputStates(MasterInputState* pState)
{
    // 0x0042057a-0x004206e7: keyboard state transitions.
    UpdateKeyboardInputState(pState);

    DWORD oldPrev = pState->keyboardPrev;
    pState->keyboardRepeat   = oldPrev;
    pState->keyboardPrev     = pState->keyboardCurr;
    pState->frameFlag        = 1;
    pState->keyboardNewPress = (~oldPrev) & pState->keyboardCurr;

    JoystickEntry* pJoy = &pState->joysticks[0];
    DWORD padMask = BuildPadMask();
    if (padMask != 0 || PadPresent()) {
        pJoy->enabled   = 1;
        pJoy->prevPress = pJoy->currPress;
        pJoy->currPress = padMask;
        pJoy->newPress  = (~pJoy->prevPress) & padMask;
    } else {
        pJoy->enabled   = 0;
        pJoy->prevPress = 0;
        pJoy->currPress = 0;
        pJoy->newPress  = 0;
    }
}

void CMarniDirectInput::InitJoysticks(MasterInputState* pState)
{
    pState->joystickCount = PadPresent() ? 1u : 0u;
}

bool MarniPadIsConnected(void)
{
    return PadPresent();
}

namespace MarniXInput {
bool IsConnected() { return PadPresent(); }
}
