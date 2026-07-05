// autopilot.h
//
// Scripted input injection + automated screenshots for headless testing and
// demos. Driven by environment variables so no code changes are needed to
// script a run:
//
//   WOLFPT_MODE=classic|pt|split|wipe|freeze   initial render mode
//   WOLFPT_SCRIPT="1000 key enter; 2000 key down; 2500 key enter; \
//                  6000 mode pt; 7000 shot; 8000 quit"
//       Time is milliseconds from launch. Actions:
//         key <name>   press a key briefly (enter,esc,up,down,left,right,
//                      space,ctrl,alt,y,n,a..z,0..9,f1..f12)
//         hold <name>  press and keep held until 'release <name>'
//         release <name>
//         mode <name>  set render mode (classic/pt/split/wipe/freeze)
//         shot         export classic+pathtraced+split screenshots
//         quit         quit cleanly
//
// AUTO_Tick() is called from both the menu input pump and the 3D frame hook so
// the schedule advances everywhere.

#ifndef WOLFPT_AUTOPILOT_H
#define WOLFPT_AUTOPILOT_H

#ifdef __cplusplus
extern "C" {
#endif

void AUTO_Init(void);      // parse env, record start time
void AUTO_Tick(void);      // advance schedule (idempotent, cheap)
int  AUTO_Active(void);    // 1 if a script is loaded

#ifdef __cplusplus
}
#endif

#endif // WOLFPT_AUTOPILOT_H
