/* Exercise the real GameController snapshot -> DirectInput keyboard boundary.
 * This does not claim tutorial progression or physical headset validation. */
#include "host.h"
#include "gamecontroller.h"
#include <assert.h>
#include <stdlib.h>
extern uint8_t host_keyboard_state[256];
extern uint8_t host_mouse_buttons[8];
void host_dinput_mouse_buttons_state(uint8_t out[8]);
void menu_probe_present(void) {}
int main(void) {
    engine_flat_base = calloc(1, 8 * 1024 * 1024);
    assert(engine_flat_base);
    HostGCSnapshot pad = {.connected = true};
    uint8_t keys[256];
    hostgc_inject_test_snapshot(&pad);
    host_dinput_keyboard_state(keys);
    assert(keys[0x12] == 0);
    pad.buttons[HOSTGC_BTN_X] = true;
    hostgc_inject_test_snapshot(&pad);
    host_dinput_keyboard_state(keys);
    assert(keys[0x12] == 0x80 && keys[0x1C] == 0);
    pad.buttons[HOSTGC_BTN_X] = false;
    hostgc_inject_test_snapshot(&pad);
    host_dinput_keyboard_state(keys);
    assert(keys[0x12] == 0);
    pad.buttons[HOSTGC_BTN_X] = true;
    pad.connected = false;
    hostgc_inject_test_snapshot(&pad);
    host_dinput_keyboard_state(keys);
    assert(keys[0x12] == 0);
    host_keyboard_state[0x12] = 0x80;
    host_dinput_keyboard_state(keys);
    assert(keys[0x12] == 0x80); /* Preserve an independently held physical E. */
    host_keyboard_state[0x12] = 0;
    pad.connected = true;
    pad.buttons[HOSTGC_BTN_A] = true;
    S8(0x00718FC9, 1);
    hostgc_inject_test_snapshot(&pad);
    host_dinput_keyboard_state(keys);
    assert(keys[0x12] == 0 && keys[0x1C] == 0x80);
    pad.buttons[HOSTGC_BTN_MENU] = true;
    S8(0x00718FC9, 0);
    hostgc_inject_test_snapshot(&pad);
    host_dinput_keyboard_state(keys);
    assert(keys[0x01] == 0x80 && keys[0x1C] == 0 && keys[0x12] == 0x80);

    const struct { int button, key; } bindings[] = {
        {HOSTGC_BTN_A,0x39}, {HOSTGC_BTN_B,0x1D}, {HOSTGC_BTN_LTHUMB,0x1D},
        {HOSTGC_BTN_X,0x12}, {HOSTGC_BTN_X,0x13}, {HOSTGC_BTN_Y,0x0F},
        {HOSTGC_BTN_RSHOULDER,0x21}, {HOSTGC_BTN_LTRIGGER,0x2C},
        {HOSTGC_BTN_RTHUMB,0x2C}, {HOSTGC_BTN_DPAD_UP,0x10},
        {HOSTGC_BTN_DPAD_DOWN,0x22}, {HOSTGC_BTN_DPAD_LEFT,0x13},
        {HOSTGC_BTN_DPAD_RIGHT,0x2D}, {HOSTGC_BTN_OPTIONS,0x3B},
    };
    for (size_t i=0; i<sizeof bindings/sizeof bindings[0]; ++i) {
        memset(&pad,0,sizeof pad); pad.connected=true;
        pad.buttons[bindings[i].button]=true;
        hostgc_inject_test_snapshot(&pad);
        host_dinput_keyboard_state(keys);
        assert(keys[bindings[i].key]==0x80 && keys[0x1C]==0);
        pad.buttons[bindings[i].button]=false;
        hostgc_inject_test_snapshot(&pad);
        host_dinput_keyboard_state(keys);
        assert(keys[bindings[i].key]==0);
    }
    uint8_t mouse[8];
    pad.buttons[HOSTGC_BTN_LTRIGGER]=true; pad.lt=1;
    hostgc_inject_test_snapshot(&pad);
    host_dinput_mouse_buttons_state(mouse);
    assert(mouse[0]==0 && mouse[1]==0); /* Aim must never throw a grenade. */
    pad.buttons[HOSTGC_BTN_RTRIGGER]=true; pad.buttons[HOSTGC_BTN_LSHOULDER]=true;
    hostgc_inject_test_snapshot(&pad);
    host_dinput_mouse_buttons_state(mouse);
    assert(mouse[0]==0x80 && mouse[1]==0x80);
    pad.connected=false;
    hostgc_inject_test_snapshot(&pad);
    host_dinput_mouse_buttons_state(mouse);
    assert(mouse[0]==0 && mouse[1]==0);
    host_mouse_buttons[0]=0x80;
    host_dinput_mouse_buttons_state(mouse);
    assert(mouse[0]==0x80);
    host_mouse_buttons[0]=0;
    pad.connected=true;
    for (int b=0;b<HOSTGC_BUTTON_COUNT;b++) pad.buttons[b]=true;
    S16(0x00718FA6,1); /* A gameplay pause widget, with shell flag still zero. */
    hostgc_inject_test_snapshot(&pad);
    host_dinput_keyboard_state(keys);
    host_dinput_mouse_buttons_state(mouse);
    assert(keys[0x1C]==0x80 && keys[0x01]==0x80);
    for (size_t i=0;i<sizeof bindings/sizeof bindings[0];i++) assert(keys[bindings[i].key]==0);
    assert(mouse[0]==0 && mouse[1]==0);
    S16(0x00718FA6,0);
    setenv("HALO_PAD2KEY","0",1);
    host_dinput_keyboard_state(keys);
    host_dinput_mouse_buttons_state(mouse);
    for (size_t i=0;i<sizeof bindings/sizeof bindings[0];i++) assert(keys[bindings[i].key]==0);
    assert(mouse[0]==0 && mouse[1]==0);
    unsetenv("HALO_PAD2KEY");
    free(engine_flat_base);
    engine_flat_base = NULL;
    puts("PASS: all gameplay button mappings, press/release, disconnect, physical-input preservation, pause-menu isolation, trigger separation, and bridge opt-out.");
    return 0;
}
