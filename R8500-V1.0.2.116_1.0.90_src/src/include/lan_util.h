/*this header file is used for R7800/8500*/
#ifndef _LAN_UTIL_H_
#define _LAN_UTIL_H_

    #define ROBO_LAN_PORTMAP            (0x1E)
    #define ROBO_WAN_PORTMAP            (0x1)
    #define ROBO_LAN_PORT_IDX_START     (1)
    #define ROBO_LAN_PORT_IDX_END       (4)
    #define ROBO_WAN_PORT               (0)

    #define ROBO_PORT_TO_LABEL_PORT(a)  ((a))
    #define LABEL_PORT_TO_ROBO_PORT(a)  ((a))

    
    #define EXT_LABEL_PORT_START    (4)
    #define EXT_LABEL_PORT_END      (6)

    #define EXT_ROBO_LAN_PORT_IDX_START     (2)
    #define EXT_ROBO_LAN_PORT_IDX_END       (4)

    #define EXT_LABEL_PORT_TO_ROBO_PORT(a)   (( a < EXT_LABEL_PORT_START)?a:(a-2))

    /*switch 1 port 4 is connect to switch 2 port 1*/
    /*0x1C => 1 1100 */
    #define EXT_ROBO_LAN_PORTMAP        (0x1C)
    /*0xE => 1110 */
    #define ROBO_LAN_PORTMAP_FOR_EXT    (0xE)

    #define EXT_PORT_SHIFT      (5)


/*foxconn Han edited, 08/15/2015 there is duplicate one in ~/swresetd/swreset_config.h */
typedef enum _LED_CONTROL_STATE {
    ENABLE_BLINK = 1,
    DISABLE_BLINK,
    TURN_OFF
} LED_CONTROL_STATE;

//extern u_int32_t _bond_xmit_hash (u_int8_t smac, u_int8_t dmac, u_int16_t pType, u_int32_t sip, u_int32_t dip);

#endif /*_LAN_UTIL_H_*/
