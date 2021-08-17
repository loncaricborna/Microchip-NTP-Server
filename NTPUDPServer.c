/******************************************************************************
 * File: NTPUDPClient.c
 * 
 * Authors: Microchip; re-assembled for the needs of ELAK d.o.o. 
 *          (Borna Loncaric)
 * 
 * Description:
 * 
 * Can be integrated into a PIC powered unit with a crystal oscillator and a 
 * network interface to provide NTP packets.
 *****************************************************************************/
/*****************************************************************************
 * PROTOTYPE
 *****************************************************************************/

#define __SNTP_C

#include <app.h>
#include "TCPIPConfig.h"
#include "TCPIP_Stack/TCPIP.h"

#define NTP_QUERY_INTERVAL		(1200ull * TICK_SECOND)
#define NTP_FAST_QUERY_INTERVAL	(20ull * TICK_SECOND)
#define NTP_REPLY_TIMEOUT		(6ull * TICK_SECOND)
#define NTP_MAX_CONNECTIONS     150
#define CMND_RX_SIZE  48       // Size of an NTP packet IN BYBTES (384 bits) (without headers!)
#define BYTES_PER_DWORD         4

#ifdef WIFI_NET_TEST
#define NTP_SERVER	"ntp" WIFI_NET_TEST_DOMAIN // NOT TESTED

#endif

/*****************************************************************************
 * NTP Packet structure
 *****************************************************************************/

typedef struct {

    struct {
        BYTE mode : 3; // NTP mode (CLIENT=3, SERVER=4)
        BYTE versionNumber : 3; // SNTP version number
        BYTE leapIndicator : 2; // Leap second indicator - set 0 for server
    } flags; // Flags for the packet

    BYTE stratum; // Stratum level of local clock                               --> 1 for server
    CHAR poll; // Poll interval
    CHAR precision; // Precision (seconds to nearest power of 2)

    // Root delay between local machine and server
    struct {
        SHORT delay_secs;
        SHORT delay_fraq;
    } root_delay;
    
    // Root dispersion (maximum error)
    struct {
        SHORT dispersion_secs;
        SHORT dispersion_fraq;
    } root_dispersion;
    
    // Reference clock identifier
    struct {
        BYTE firstLetter : 8;
        BYTE secondLetter : 8;
        BYTE thirdLetter : 8;
        BYTE otherbytes : 8;
    } ref_identifier;   // tested 18:40 26.6.2019. (implemented as DWORD, 4-octets)

    DWORD ref_ts_secs; // Reference timestamp (in seconds)
    DWORD ref_ts_fraq; // Reference timestamp (fractions)
    DWORD orig_ts_secs; // Origination timestamp (in seconds)
    DWORD orig_ts_fraq; // Origination timestamp (fractions)
    DWORD recv_ts_secs; // Time at which request arrived at sender (seconds)
    DWORD recv_ts_fraq; // Time at which request arrived at sender (fractions)
    DWORD tx_ts_secs; // Time at which request left sender (seconds)
    DWORD tx_ts_fraq; // Time at which request left sender (fractions)
} NTP_PACKET;

typedef struct {
    int recv_tick;
    int orig_tick;
    int tx_tick;
} DELAY_TICK_COUNT;

/******************************************************************************/

// Here a structure of choice to support time information
static mainTime_t ntpTime;

UINT32 dwTimeKeeper;

static enum {
    SM_NTP_CREATE_SOCKET,
    SM_NTP_UDP_BIND,	// Not required in CLIENT since we are sending the first packet
    SM_NTP_UDP_LISTENING,
    SM_NTP_UDP_ANSWER,
} SNTPState;

/*****************************************************************************
 * Restart & Close functions
 *****************************************************************************/

void NTPUDPServer_Restart(void) {
    SNTPState = SM_NTP_CREATE_SOCKET;
}

void NTPUDPServer_Close(void) {
    // Was unnecessary at the moment of creation
}

/*******************************************************************************
 * Read unsingned 32bit int
 ******************************************************************************/

static UINT32 ntp_read_u32(UINT32 val) {
    UINT8 tmp;
    UINT8 *d;

    d = (UINT8*) & val;
    tmp = d[0];
    d[0] = d[3];
    d[3] = tmp;
    tmp = d[1];
    d[1] = d[2];
    d[2] = tmp;
    return val;
}

/*******************************************************************************
 * Server loop (!)
 * 
 * Here the server grabs the request, fills the sending packet with necessary
 * information and sends it back to the client. Here a couple more libraries
 * are used, of which notable are Microchip DNS library, UDP library and 
 * Berkeley API to move past the lower layers of the OSI model. Here only
 * the UDP layer is taken care of.
 ******************************************************************************/

void NTPUDPServer(void) {
    
#if defined(STACK_USE_DNS)
    static NTP_PACKET pkt;
    static NTP_PACKET pktRx;
    static DWORD dwTimer;
    static DWORD currentlyActiveIP;
    static SOCKET bsdUdpServerSocket;       // Berkeley API socket spawn
    static int addrlen = sizeof (struct sockaddr_in);
    static struct sockaddr_in udpaddr;
    static int bindConfirmation;
    static int rxInfo;
    static UINT32 actualNTPTime;
    static LONG originFraction;
    static LONG transmitFraction;
    static DWORD delayShift;

    /***************************************************************************
     * MAIN SWITCH-CASE
     **************************************************************************/
    
    delayShift = gps_msDelay;

    switch (SNTPState) {  
        
        /*******************************************************************************
         *                           CREATION of the socket
         ******************************************************************************/
        
        case SM_NTP_CREATE_SOCKET:

            currentlyActiveIP = ntpsLastKnownIP;
            
                // Allocate a socket for this server to listen and accept connections on
            bsdUdpServerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                
            if (bsdUdpServerSocket != INVALID_SOCKET && bsdUdpServerSocket != UNKNOWN_SOCKET) {
                SNTPState = SM_NTP_UDP_BIND;
                break;
            }
            break;
        
        /*******************************************************************************
         *                                   BINDING
         ******************************************************************************/
            
        case SM_NTP_UDP_BIND:
            
            udpaddr.sin_family = AF_INET;   // IPv4
            udpaddr.sin_port = AppConfig.ntpPort;   //123
            udpaddr.sin_addr.S_un.S_addr = IP_ADDR_ANY;  // Zaprimanje upita s bilo koje adrese na mrezi
            
                // Bind the socket
            bindConfirmation = bind(bsdUdpServerSocket, (struct sockaddr*) &udpaddr, addrlen);
                
            if (bindConfirmation == 0) {
                // If the binding is affirmative, proceed to listening operation
                SNTPState = SM_NTP_UDP_LISTENING;
            } else if (bindConfirmation != 0 || currentlyActiveIP != ntpsLastKnownIP
                    || bsdUdpServerSocket==INVALID_SOCKET) {
                SNTPState = SM_NTP_CREATE_SOCKET;
                break;
            }

            break;
        
        /*******************************************************************************
         *                              LISTENING socket
         ******************************************************************************/

        case SM_NTP_UDP_LISTENING:
        {
            // Provjera ako postoji otvoren socket za zaprimanje NTP upitnih paketa
            if (UDPIsOpened (bsdUdpServerSocket) == false) {
                SNTPState = SM_NTP_CREATE_SOCKET;
                break;
            }
            
            if (currentlyActiveIP != ntpsLastKnownIP) {
                closesocket(bsdUdpServerSocket);
                SNTPState = SM_NTP_CREATE_SOCKET;
                break;
            }
            
                // rxInfo je varijabla provjere za ocitanje upitnog paketa
            rxInfo = recvfrom(bsdUdpServerSocket, (signed char*) &pktRx, sizeof (pktRx), 0, (struct sockaddr*) &udpaddr, &addrlen);

            if (rxInfo != 0) {

                    // Fill the RECEIVED TIMESTAMP INFO
                
                    // Receive timestamp
                pktRx.recv_ts_secs = ntp_read_u32(actualNTPTime); //swapl(dwOURSeconds);
                pktRx.recv_ts_fraq = arithmeticFraction;//delayShift;//TickGetDiv64K();  // CHANGED FROM TickGet();
                
                originFraction = TickGet();

                SNTPState = SM_NTP_UDP_ANSWER;

            }
            
            break;
        }

        /*******************************************************************************
         *                               ANSWER packet
         ******************************************************************************/
        case SM_NTP_UDP_ANSWER:            
            // Transmit a time packet on request
            memset(&pkt, 0, sizeof (pkt));

            /************ 1. row **************/
            //pkt.flags.leapIndicator = 0; // 00 = no warning
            pkt.flags.versionNumber = 3; // NTP Version 3
            pkt.flags.mode = 4; // NTP Server=4
            /***/            
            pkt.stratum = 1;
            pkt.poll = 6;
            pkt.precision = -9; // Set As default?
            /*--------------------------------*/

            /************ 2. row **************/

            if (AppConfig.gpsIsEnabled == 1 && gpsLastSyncFail == false) { // if GPS is plugged in
                pkt.flags.leapIndicator = 0; // 00 = no warning
            } else if (isClockManual == true) {
                pkt.flags.leapIndicator = 0;    // If the unit is manually synced
            } else {
                pkt.flags.leapIndicator = 3;    // If the unit is not synchronized
            }                     
            
            /* ROOT DELAY IS ADDED AT THE END */
                    
            /***********Ref_ID fill************/    // Added by Borna, 26.6.2019.

            // Literal variable "GPS " which was used in this unit
            pkt.ref_identifier.firstLetter = 0x47;
            pkt.ref_identifier.secondLetter = 0x50;
            pkt.ref_identifier.thirdLetter = 0x53;
            pkt.ref_identifier.otherbytes = 0x00;
            
            /*--------------------------------*/

            /************ 3. row **************/


                //-------

            /************* 4. row *************/

            if (AppConfig.gpsIsEnabled == 1 && gpsLastSyncFail == false) { // if GPS is plugged in
                pkt.flags.leapIndicator = 0; // 00 = no warning
            } else if (isClockManual == true) {
                pkt.flags.leapIndicator = 0;    // If the unit is manually synced
            } else {
                pkt.flags.leapIndicator = 3;    // If the unit is not synchronized
            }
            
            pkt.ref_ts_secs = ntp_read_u32(actualNTPTime);
            pkt.ref_ts_fraq = arithmeticFraction;//delayShift;//TickGetDiv64K();  // CHANGED FROM TickGet();)

            /*--------------------------------*/

            /************* 5. row *************/

                // Filled by client --> origin = client tx
            pkt.orig_ts_secs = pktRx.tx_ts_secs;
            pkt.orig_ts_fraq = pktRx.tx_ts_fraq;
            
            /*--------------------------------*/

            /************* 6. row *************/

            pkt.recv_ts_secs = pktRx.recv_ts_secs;
            pkt.recv_ts_fraq = pktRx.recv_ts_fraq;

            /*--------------------------------*/

            /************* 7. row *************/

            // Last time the time is read and filled as transmit timestamp before sending the packet
            
            if (AppConfig.gpsIsEnabled == 1 && gpsLastSyncFail == false) { // if GPS is plugged in
                pkt.flags.leapIndicator = 0; // 00 = no warning
            } else if (isClockManual == true) {
                pkt.flags.leapIndicator = 0;    // If the unit is manually synced
            } else {
                pkt.flags.leapIndicator = 3;    // If the unit is not synchronized
            }
                // Transmit Timestamp
            pkt.tx_ts_secs = ntp_read_u32(actualNTPTime); //swapl(dwOURSeconds);
            pkt.tx_ts_fraq = arithmeticFraction;//delayShift;//TickGetDiv64K();     // returns fractions from Tick_T1 class
            
            
            /*--------------------------------*/

            /************* Root delay calculation *************/
                // Root Delay obracun

            pkt.root_delay.delay_secs = 0;
            transmitFraction = TickGet();
            pkt.root_delay.delay_fraq = (transmitFraction - originFraction)%1000;
            
            /*---------------END OF ROOT DELAY CALC-----------------*/
            
            pkt.root_dispersion.dispersion_secs = 0;//(pkt.tx_ts_secs - pkt.ref_ts_secs); //--> zero seconds
            pkt.root_dispersion.dispersion_fraq = 0x11/16%100;  // Crystal aging PPM by DS3231 datasheet

            if (sendto(bsdUdpServerSocket, (const signed char*) &pkt, sizeof (pkt), 0, (struct sockaddr*) &udpaddr, addrlen) > 0) {
                dwTimer = TickGet();
                SNTPState = SM_NTP_UDP_LISTENING;
            }
            break;
            /*******************************************************************************
             *                            END of ANSWER packet
             ******************************************************************************/
    }

#else
#warning You must define STACK_USE_DNS for NTP Server to work.
#endif
}