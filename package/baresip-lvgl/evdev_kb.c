#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <ctype.h>
#include "evdev_kb.h"

int evdev_kb_fd = -1;
int evdev_kb_key_val = 0;
int evdev_kb_state = LV_INDEV_STATE_REL;
static int shift_active = 0;

void evdev_kb_init(void)
{ 
    puts("EVDEV_KB: evdev_kb_init called - Auto-detecting KEYBOARD..."); fflush(stdout);
    
    char dev_path[32];
    char best_dev_path[32];
    best_dev_path[0] = '\0';
    int found = 0;
    int i;

    /* Scan /dev/input/event0 to event9 */
    for (i = 0; i < 10; i++) {
        sprintf(dev_path, "/dev/input/event%d", i);
        int fd = open(dev_path, O_RDONLY);
        if (fd >= 0) {
            unsigned long ev_bits[1] = {0};
            
            #ifndef EVIOCGBIT
            #define EVIOCGBIT(ev,len)  _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
            #endif

            if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0) {
                 int has_key = (ev_bits[0] >> EV_KEY) & 1;
                 int has_rel = (ev_bits[0] >> EV_REL) & 1;
                 int has_abs = (ev_bits[0] >> EV_ABS) & 1;

                 // Logic: If it has keys but NO relative/absolute pointers, it's likely a keyboard
                 if (has_key && !has_rel && !has_abs) {
                     printf("EVDEV_KB: Found Candidate %s (Type=Key)\n", dev_path);
                     sprintf(best_dev_path, "%s", dev_path);
                     found = 1;
                     close(fd);
                     break; 
                 }
            }
            close(fd);
        }
    }

    if (found) {
        printf("EVDEV_KB: Selected device: %s\n", best_dev_path);
        
        if(evdev_kb_fd != -1) close(evdev_kb_fd);
        evdev_kb_fd = open(best_dev_path, O_RDWR | O_NOCTTY | O_NDELAY);
        if(evdev_kb_fd != -1) {
             fcntl(evdev_kb_fd, F_SETFL, O_ASYNC | O_NONBLOCK);
             puts("EVDEV_KB: Open success");
        } else {
             perror("EVDEV_KB: Open failed");
        }
    } else {
        printf("EVDEV_KB: No dedicated keyboard found (using default event0?)\n");
    }
}

void evdev_kb_read(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    (void)drv; // Unused
    struct input_event in;

    while(read(evdev_kb_fd, &in, sizeof(struct input_event)) > 0) {
        if(in.type == EV_KEY) {
            
            // Handle Shift Keys globally
            if (in.code == KEY_LEFTSHIFT || in.code == KEY_RIGHTSHIFT) {
                shift_active = (in.value == 1 || in.value == 2); // 1=Press, 2=Repeat, 0=Release
                continue;
            }

            // Ignore mouse buttons
            if(in.code == BTN_MOUSE || in.code == BTN_TOUCH) continue;

             data->state = (in.value) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
             data->key = 0;

             // Map Keys
             switch(in.code) {
                case KEY_BACKSPACE: data->key = LV_KEY_BACKSPACE; break;
                case KEY_ENTER: data->key = LV_KEY_ENTER; break;
                case KEY_ESC: data->key = LV_KEY_ESC; break;
                case KEY_UP: data->key = LV_KEY_UP; break;
                case KEY_DOWN: data->key = LV_KEY_DOWN; break;
                case KEY_LEFT: data->key = LV_KEY_LEFT; break;
                case KEY_RIGHT: data->key = LV_KEY_RIGHT; break;
                case KEY_TAB: data->key = LV_KEY_NEXT; break;
                case KEY_DELETE: data->key = LV_KEY_DEL; break;
                case KEY_HOME: data->key = LV_KEY_HOME; break;
                case KEY_END: data->key = LV_KEY_END; break;
                
                // QWERTY Row 1
                case KEY_Q: data->key = shift_active ? 'Q' : 'q'; break;
                case KEY_W: data->key = shift_active ? 'W' : 'w'; break;
                case KEY_E: data->key = shift_active ? 'E' : 'e'; break;
                case KEY_R: data->key = shift_active ? 'R' : 'r'; break;
                case KEY_T: data->key = shift_active ? 'T' : 't'; break;
                case KEY_Y: data->key = shift_active ? 'Y' : 'y'; break;
                case KEY_U: data->key = shift_active ? 'U' : 'u'; break;
                case KEY_I: data->key = shift_active ? 'I' : 'i'; break;
                case KEY_O: data->key = shift_active ? 'O' : 'o'; break;
                case KEY_P: data->key = shift_active ? 'P' : 'p'; break;
                case KEY_LEFTBRACE: data->key = shift_active ? '{' : '['; break;
                case KEY_RIGHTBRACE: data->key = shift_active ? '}' : ']'; break;
                case KEY_BACKSLASH: data->key = shift_active ? '|' : '\\'; break;

                // QWERTY Row 2
                case KEY_A: data->key = shift_active ? 'A' : 'a'; break;
                case KEY_S: data->key = shift_active ? 'S' : 's'; break;
                case KEY_D: data->key = shift_active ? 'D' : 'd'; break;
                case KEY_F: data->key = shift_active ? 'F' : 'f'; break;
                case KEY_G: data->key = shift_active ? 'G' : 'g'; break;
                case KEY_H: data->key = shift_active ? 'H' : 'h'; break;
                case KEY_J: data->key = shift_active ? 'J' : 'j'; break;
                case KEY_K: data->key = shift_active ? 'K' : 'k'; break;
                case KEY_L: data->key = shift_active ? 'L' : 'l'; break;
                case KEY_SEMICOLON: data->key = shift_active ? ':' : ';'; break;
                case KEY_APOSTROPHE: data->key = shift_active ? '"' : '\''; break;

                // QWERTY Row 3
                case KEY_Z: data->key = shift_active ? 'Z' : 'z'; break;
                case KEY_X: data->key = shift_active ? 'X' : 'x'; break;
                case KEY_C: data->key = shift_active ? 'C' : 'c'; break;
                case KEY_V: data->key = shift_active ? 'V' : 'v'; break;
                case KEY_B: data->key = shift_active ? 'B' : 'b'; break;
                case KEY_N: data->key = shift_active ? 'N' : 'n'; break;
                case KEY_M: data->key = shift_active ? 'M' : 'm'; break;
                case KEY_COMMA: data->key = shift_active ? '<' : ','; break;
                case KEY_DOT: data->key = shift_active ? '>' : '.'; break;
                case KEY_SLASH: data->key = shift_active ? '?' : '/'; break;
                
                // Numbers & Symbols
                case KEY_GRAVE: data->key = shift_active ? '~' : '`'; break;
                case KEY_1: data->key = shift_active ? '!' : '1'; break;
                case KEY_2: data->key = shift_active ? '@' : '2'; break;
                case KEY_3: data->key = shift_active ? '#' : '3'; break;
                case KEY_4: data->key = shift_active ? '$' : '4'; break;
                case KEY_5: data->key = shift_active ? '%' : '5'; break;
                case KEY_6: data->key = shift_active ? '^' : '6'; break;
                case KEY_7: data->key = shift_active ? '&' : '7'; break;
                case KEY_8: data->key = shift_active ? '*' : '8'; break;
                case KEY_9: data->key = shift_active ? '(' : '9'; break;
                case KEY_0: data->key = shift_active ? ')' : '0'; break;
                case KEY_MINUS: data->key = shift_active ? '_' : '-'; break;
                case KEY_EQUAL: data->key = shift_active ? '+' : '='; break; 
                
                case KEY_SPACE: data->key = ' '; break;
                
                // Numpad Support (Optional but good)
                case KEY_KP1: data->key = '1'; break;
                case KEY_KP2: data->key = '2'; break;
                case KEY_KP3: data->key = '3'; break;
                case KEY_KP4: data->key = '4'; break;
                case KEY_KP5: data->key = '5'; break;
                case KEY_KP6: data->key = '6'; break;
                case KEY_KP7: data->key = '7'; break;
                case KEY_KP8: data->key = '8'; break;
                case KEY_KP9: data->key = '9'; break;
                case KEY_KP0: data->key = '0'; break;
                case KEY_KPDOT: data->key = '.'; break;
                case KEY_KPPLUS: data->key = '+'; break;
                case KEY_KPMINUS: data->key = '-'; break;
                case KEY_KPASTERISK: data->key = '*'; break;
                case KEY_KPSLASH: data->key = '/'; break;
                case KEY_KPENTER: data->key = LV_KEY_ENTER; break;

                default:
                    data->key = 0; 
                    break;
            }
            
            if (data->key != 0) {
                evdev_kb_key_val = data->key;
                evdev_kb_state = data->state;
                return;
            }
        }
    }

    data->key = evdev_kb_key_val;
    data->state = evdev_kb_state;
}
