// No joystick or gamepad on this machine.  The variables still bind so a
// config file carrying them round-trips unchanged.

#include "doomtype.h"
#include "i_joystick.h"
#include "m_config.h"

int use_analog = 0;
int joystick_turn_sensitivity = 10;
int joystick_move_sensitivity = 10;
int joystick_look_sensitivity = 10;

void I_InitJoystick(void) {}
void I_ShutdownJoystick(void) {}
void I_UpdateJoystick(void) {}

void
I_BindJoystickVariables(void)
{
    M_BindIntVariable("use_analog", &use_analog);
    M_BindIntVariable("joystick_turn_sensitivity", &joystick_turn_sensitivity);
    M_BindIntVariable("joystick_move_sensitivity", &joystick_move_sensitivity);
    M_BindIntVariable("joystick_look_sensitivity", &joystick_look_sensitivity);
}
