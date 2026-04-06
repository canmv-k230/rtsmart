#ifndef RT_INPUT_EVENT_H
#define RT_INPUT_EVENT_H

#include <rtthread.h>
#include <rtdevice.h>

#define INPUT_EVENT_RX_BUFSIZE 1024

/* Event types */
#define EV_SYN                  0x00
#define EV_KEY                  0x01
#define EV_REL                  0x02
#define EV_ABS                  0x03

/* Synchronization events */
#define SYN_REPORT              0

/* Key values */
#define KEY_RELEASED            0
#define KEY_PRESSED             1
#define KEY_REPEAT              2

/* Linux keycodes used by HID keyboards */
#define KEY_RESERVED            0
#define KEY_ESC                 1
#define KEY_1                   2
#define KEY_2                   3
#define KEY_3                   4
#define KEY_4                   5
#define KEY_5                   6
#define KEY_6                   7
#define KEY_7                   8
#define KEY_8                   9
#define KEY_9                   10
#define KEY_0                   11
#define KEY_MINUS               12
#define KEY_EQUAL               13
#define KEY_BACKSPACE           14
#define KEY_TAB                 15
#define KEY_Q                   16
#define KEY_W                   17
#define KEY_E                   18
#define KEY_R                   19
#define KEY_T                   20
#define KEY_Y                   21
#define KEY_U                   22
#define KEY_I                   23
#define KEY_O                   24
#define KEY_P                   25
#define KEY_LEFTBRACE           26
#define KEY_RIGHTBRACE          27
#define KEY_ENTER               28
#define KEY_LEFTCTRL            29
#define KEY_A                   30
#define KEY_S                   31
#define KEY_D                   32
#define KEY_F                   33
#define KEY_G                   34
#define KEY_H                   35
#define KEY_J                   36
#define KEY_K                   37
#define KEY_L                   38
#define KEY_SEMICOLON           39
#define KEY_APOSTROPHE          40
#define KEY_GRAVE               41
#define KEY_LEFTSHIFT           42
#define KEY_BACKSLASH           43
#define KEY_Z                   44
#define KEY_X                   45
#define KEY_C                   46
#define KEY_V                   47
#define KEY_B                   48
#define KEY_N                   49
#define KEY_M                   50
#define KEY_COMMA               51
#define KEY_DOT                 52
#define KEY_SLASH               53
#define KEY_RIGHTSHIFT          54
#define KEY_KPASTERISK          55
#define KEY_LEFTALT             56
#define KEY_SPACE               57
#define KEY_CAPSLOCK            58
#define KEY_F1                  59
#define KEY_F2                  60
#define KEY_F3                  61
#define KEY_F4                  62
#define KEY_F5                  63
#define KEY_F6                  64
#define KEY_F7                  65
#define KEY_F8                  66
#define KEY_F9                  67
#define KEY_F10                 68
#define KEY_NUMLOCK             69
#define KEY_SCROLLLOCK          70
#define KEY_KP7                 71
#define KEY_KP8                 72
#define KEY_KP9                 73
#define KEY_KPMINUS             74
#define KEY_KP4                 75
#define KEY_KP5                 76
#define KEY_KP6                 77
#define KEY_KPPLUS              78
#define KEY_KP1                 79
#define KEY_KP2                 80
#define KEY_KP3                 81
#define KEY_KP0                 82
#define KEY_KPDOT               83
#define KEY_F11                 87
#define KEY_F12                 88
#define KEY_KPENTER             96
#define KEY_RIGHTCTRL           97
#define KEY_KPSLASH             98
#define KEY_SYSRQ               99
#define KEY_RIGHTALT            100
#define KEY_HOME                102
#define KEY_UP                  103
#define KEY_PAGEUP              104
#define KEY_LEFT                105
#define KEY_RIGHT               106
#define KEY_END                 107
#define KEY_DOWN                108
#define KEY_PAGEDOWN            109
#define KEY_INSERT              110
#define KEY_DELETE              111
#define KEY_LEFTMETA            125
#define KEY_RIGHTMETA           126

/* Relative axes */
#define REL_X                   0x00
#define REL_Y                   0x01
#define REL_HWHEEL              0x06
#define REL_WHEEL               0x08

/* Absolute axes */
#define ABS_X                   0x00
#define ABS_Y                   0x01
#define ABS_PRESSURE            0x18

/* Buttons */
#define BTN_MOUSE               0x110
#define BTN_LEFT                0x110
#define BTN_RIGHT               0x111
#define BTN_MIDDLE              0x112
#define BTN_SIDE                0x113
#define BTN_EXTRA               0x114
#define BTN_FORWARD             0x115
#define BTN_BACK                0x116
#define BTN_TOUCH               0x14a

#define RT_INPUT_NAME_MAX       32

enum rt_input_kind
{
    RT_INPUT_KIND_UNKNOWN = 0,
    RT_INPUT_KIND_KEYBOARD,
    RT_INPUT_KIND_MOUSE,
    RT_INPUT_KIND_TOUCH,
};

struct rt_input_info
{
    rt_uint32_t kind;
    rt_uint32_t ev_bits;
    rt_uint32_t key_bits;
    rt_uint32_t rel_bits;
    rt_uint32_t abs_bits;
    char name[RT_INPUT_NAME_MAX];
};

#define RT_INPUT_CTRL_GET_INFO  0x1001

struct input_event
{
    rt_uint16_t type;
    rt_uint16_t code;
    rt_int32_t value;
};

struct rt_input_rx_fifo
{
    struct rt_ringbuffer rb;
    rt_uint8_t buffer[];
};

struct rt_input_dev
{
    struct rt_device parent;
    struct rt_input_rx_fifo *rx_fifo;
    rt_size_t rx_bufsz;
    int id;
    rt_bool_t connected;
    struct rt_input_info info;
};

rt_err_t rt_input_dev_register(struct rt_input_dev *input);
rt_err_t rt_input_dev_reconnect(struct rt_input_dev *input);
rt_err_t rt_input_dev_disconnect(struct rt_input_dev *input);
void rt_input_dev_unregister(struct rt_input_dev *input);
rt_err_t rt_input_report(struct rt_input_dev *input, rt_uint16_t type, rt_uint16_t code, rt_int32_t value);
rt_err_t rt_input_sync(struct rt_input_dev *input);
void rt_input_set_name(struct rt_input_dev *input, const char *name);
void rt_input_set_kind(struct rt_input_dev *input, rt_uint32_t kind);
void rt_input_set_capability(struct rt_input_dev *input, rt_uint16_t type, rt_uint16_t code);

#endif