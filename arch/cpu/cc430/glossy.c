/*
 * Copyright (c) 2016, Swiss Federal Institute of Technology (ETH Zurich).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author:  Federico Ferrari
 *          Reto Da Forno
 */

/*
 * Note: the constants required to calculate the slot duration are defined in
 * the rf1a config file (TAU1, TAU2, T2R, R2T, T_TX_BYTE and T_TX_OFFSET)
 */

#include "contiki.h"
#include "rtimer-ext.h"
#include "glossy.h"
#include "rf1a.h"
#include "gpio.h"
#include "random.h"
#include "node-id.h"

/*---------------------------------------------------------------------------*/

/* minimum and maximum number of slots after which the timeout expires, since
 * the last transmission
 * NOTE: values below 2 do not make sense, as there would be no chance to
 * receive a packet in between */
#define SLOT_TIMEOUT_MIN      2
#define SLOT_TIMEOUT_MAX      2
/* number of extra ticks required by the timeout callback before starting the
 * transmission (to keep synchronous transmissions and time synchronization as
 * accurate as possible) */
#define TIMEOUT_EXTRA_TICKS   70

/* maximum tolerance to accept measurements of T_slot when comparing to the
 * theoretical value (value in clock ticks) */
#define T_SLOT_TOLERANCE      10

#define GLOSSY_MAX_PACKET_LEN (GLOSSY_CONF_PAYLOAD_LEN + GLOSSY_MAX_HEADER_LEN)

#define GLOSSY_HEADER_BYTE_MASK   0xe0    /* 3 bits */
#define GLOSSY_HEADER_SYNC_MASK   0x10    /* 1 bit */
#define GLOSSY_HEADER_N_TX_MASK   0x0f    /* 4 bits */
#define GLOSSY_COMMON_HEADER      (GLOSSY_CONF_HEADER_BYTE & \
                                   GLOSSY_HEADER_BYTE_MASK)

#if GLOSSY_CONF_SETUPTIME_WITH_SYNC
#define GLOSSY_SYNC_SETUP_TICKS   (uint16_t)(GLOSSY_CONF_SETUPTIME_WITH_SYNC \
                                             * RTIMER_EXT_SECOND_LF / 1000000)
#endif /* GLOSSY_CONF_SETUPTIME_WITH_SYNC */

#define SET_PKT_TYPE(pkt_type, with_sync, n_tx_max) \
                        (pkt_type = GLOSSY_COMMON_HEADER | \
                                ((with_sync) ? GLOSSY_HEADER_SYNC_MASK : 0) | \
                                ((n_tx_max) & GLOSSY_HEADER_N_TX_MASK))
#define SET_SYNC(pkt_type, with_sync) \
                        (pkt_type = (pkt_type & ~GLOSSY_HEADER_SYNC_MASK) | \
                                    (with_sync ? GLOSSY_HEADER_SYNC_MASK : 0))
#define SET_N_TX_MAX(pkt_type, n_tx_max) \
                        (pkt_type = ((pkt_type) & ~GLOSSY_HEADER_N_TX_MASK) | \
                                    ((n_tx_max) & GLOSSY_HEADER_N_TX_MASK))

#define GET_COMMON_HEADER(pkt_type)   ((pkt_type) & GLOSSY_HEADER_BYTE_MASK)
#define GET_SYNC(pkt_type)            ((pkt_type) & GLOSSY_HEADER_SYNC_MASK)
#define GET_N_TX_MAX(pkt_type)        ((pkt_type) & GLOSSY_HEADER_N_TX_MASK)

#define IS_INITIATOR()    (g.initiator_id == node_id)
#define WITH_SYNC()       (GET_SYNC(g.header.pkt_type))

#if GLOSSY_CONF_ALWAYS_RELAY_CNT
#define WITH_RELAY_CNT()  (1)               /* relay counter always */
#else
#define WITH_RELAY_CNT()  (WITH_SYNC())     /* relay counter if sync enabled */
#endif /* GLOSSY_CONF_ALWAYS_RELAY_CNT */

#define GLOSSY_HEADER_LEN(pkt_type) \
                          (( (WITH_SYNC()) || (WITH_RELAY_CNT()) ) \
                              ? 2 : 1 )

/* mainly for debugging purposes */
#ifdef GLOSSY_START_PIN
#define GLOSSY_STARTED      PIN_SET(GLOSSY_START_PIN)
#define GLOSSY_STOPPED      PIN_CLR(GLOSSY_START_PIN)
#else
#define GLOSSY_STARTED
#define GLOSSY_STOPPED
#endif

#ifdef GLOSSY_RX_PIN 
#define GLOSSY_RX_STARTED   PIN_SET(GLOSSY_RX_PIN)
#define GLOSSY_RX_STOPPED   PIN_CLR(GLOSSY_RX_PIN)
#else
#define GLOSSY_RX_STARTED
#define GLOSSY_RX_STOPPED
#endif

#ifdef GLOSSY_TX_PIN
#define GLOSSY_TX_STARTED   PIN_SET(GLOSSY_TX_PIN)
#define GLOSSY_TX_STOPPED   PIN_CLR(GLOSSY_TX_PIN)
#else
#define GLOSSY_TX_STARTED
#define GLOSSY_TX_STOPPED
#endif

/* indicate the first successful reception */
#ifndef GLOSSY_FIRST_RX
#define GLOSSY_FIRST_RX
#endif /* GLOSSY_FIRST_RX */

/*---------------------------------------------------------------------------*/
typedef struct {
  uint8_t  pkt_type;
  uint8_t  relay_cnt;
} glossy_header_t;
/*---------------------------------------------------------------------------*/
typedef struct {
  rtimer_ext_clock_t  t_ref,
                  t_tx_stop,
                  t_rx_start,
                  t_rx_stop,
                  t_tx_start;
  rtimer_ext_clock_t  T_slot_sum;
  rtimer_ext_clock_t  t_timeout;
  uint32_t        T_slot_estimated;                  /* 32-bit is sufficient */
  glossy_header_t header;
  uint16_t initiator_id;
  uint8_t* payload;
  uint8_t  payload_len;
  uint8_t  n_T_slot;
  uint8_t  active;
  uint8_t  t_ref_updated;
  uint8_t  header_ok;
  uint8_t  relay_cnt_last_rx;
  uint8_t  relay_cnt_last_tx;
  uint8_t  relay_cnt_t_ref;
  uint8_t  relay_cnt_timeout;
  uint8_t  n_rx;                                /* rx counter for last flood */
  uint8_t  n_tx;
#if GLOSSY_CONF_COLLECT_STATS
  struct {
  /* --- statistics only, otherwise not relevant for Glossy --- */
  /* stats of the last flood */
  uint8_t  last_flood_relay_cnt;                    /* relay cnt on first rx */
  int8_t   last_flood_rssi_noise;
  int16_t  last_flood_rssi_sum;
  uint8_t  last_flood_n_rx_started;            /* # preamble+sync detections */
  uint8_t  last_flood_n_rx_fail;                      /* header or CRC wrong */
  uint8_t  already_counted;
  rtimer_ext_clock_t last_flood_duration;                /* total flood duration */
  rtimer_ext_clock_t last_flood_t_to_rx;              /* time to first reception */
  /* global stats since last reset of the node */
  uint32_t pkt_cnt;           /* total # of received packets (preamble+sync) */
  uint32_t pkt_cnt_crcok;         /* total # of received packets with CRC ok */
  uint32_t flood_cnt;    /* total # of floods (with >=1x preamble+sync det.) */
  uint32_t flood_cnt_success;      /* total # floods with at least 1x CRC ok */
  uint16_t error_cnt;              /* total number of errors */
  } stats;
#endif /* GLOSSY_CONF_COLLECT_STATS */
} glossy_state_t;
/*---------------------------------------------------------------------------*/
static glossy_state_t g;

/*------------------------ Glossy helper functions --------------------------*/
static inline uint8_t
process_glossy_header(uint8_t *pkt, uint8_t pkt_len, uint8_t crc_ok)
{
  /* extract the Glossy header from the packet */
  glossy_header_t *rcvd_header = (glossy_header_t*)pkt;

  if(!g.header_ok) {
    /* we have not checked the header yet, so check it now */

    if(GET_COMMON_HEADER(rcvd_header->pkt_type) != GLOSSY_COMMON_HEADER) {
      /* keep processing only if the common header is correct */
      return 0;
    }
    if(GET_SYNC(g.header.pkt_type) != GET_SYNC(rcvd_header->pkt_type)) {
      /* keep processing only if the local sync value is either unknown or it
       * matches the received one */
      return 0;
    }
    if((GET_N_TX_MAX(g.header.pkt_type) != GLOSSY_UNKNOWN_N_TX_MAX) &&
       (GET_N_TX_MAX(g.header.pkt_type) !=
        GET_N_TX_MAX(rcvd_header->pkt_type))) {
      /* keep processing only if the local n_tx_max value is either unknown or
       * it matches the received one */
      return 0;
    }
    if(((g.payload_len != GLOSSY_UNKNOWN_PAYLOAD_LEN) &&
        (g.payload_len != (pkt_len - GLOSSY_HEADER_LEN(g.header.pkt_type))))
        || (pkt_len > (GLOSSY_CONF_PAYLOAD_LEN + GLOSSY_MAX_HEADER_LEN))
       ) {
      /* keep processing only if the local payload_len value is either unknown
       * or it matches the received one */
      return 0;
    }
    if(pkt_len > (GLOSSY_CONF_PAYLOAD_LEN + GLOSSY_MAX_HEADER_LEN)) {
      return 0;
      /* keep processing only if the received packet length make sense
       * (rf1a driver had been found unreliable)*/
    }
    /* the header is ok */
    g.header_ok = 1;
  }

  if(crc_ok) {
    if(pkt_len > (GLOSSY_CONF_PAYLOAD_LEN + GLOSSY_MAX_HEADER_LEN)) {
      return 0;
      /* keep processing only if the received packet length make sense
       * (rf1a driver had been found unreliable)*/
    }
    /* we have received the entire packet (and the CRC was ok) */
    /* store the received header (all the unknown values are also learned) */
    g.header = *rcvd_header;
    /* store the payload_len */
    g.payload_len = pkt_len - GLOSSY_HEADER_LEN(g.header.pkt_type);
    /* store the header_len */
    rf1a_set_header_len_rx(GLOSSY_HEADER_LEN(g.header.pkt_type));
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
static inline uint32_t
estimate_T_slot(uint8_t pkt_len)
{
  /* T_slot = T_rx + T_rx2tx + tau1 = T_tx + T_tx2rx - tau1 */
  //rtimer_ext_clock_t T_tx_estim = T_TX_BYTE * (pkt_len + 3) + T_TX_OFFSET;
  /* perform calculations in 32-bit, faster */
  uint32_t T_tx_estim = (uint32_t)T_TX_BYTE * ((uint32_t)pkt_len + 3)
                      + (uint32_t)T_TX_OFFSET;
  return NS_TO_RTIMER_EXT_HF_32(T_tx_estim + T2R - TAU1);
}
/*---------------------------------------------------------------------------*/
static inline char
timeout_expired(rtimer_ext_t *rt)
{
  if(!rf1a_is_busy()) {
    /* we are not receiving anything: retransmit the packet */
    rf1a_start_tx();
    g.header.relay_cnt = g.relay_cnt_timeout;
    rf1a_write_to_tx_fifo((uint8_t *)&g.header,
                          GLOSSY_HEADER_LEN(g.header.pkt_type),
                          (uint8_t *)g.payload, g.payload_len);
    g.t_timeout = rt->time;
  } else {
    /* we are receiving a packet: postpone the timeout by one slot */
    g.relay_cnt_timeout++;
    rtimer_ext_schedule(GLOSSY_CONF_RTIMER_ID, rt->time + g.T_slot_estimated, 0,
                        timeout_expired);
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
static inline void
schedule_timeout(void)
{
  /* number of slots after which the timeout will expire: */
  /* random number between SLOT_TIMEOUT_MIN and SLOT_TIMEOUT_MAX */
  uint8_t slot_timeout = SLOT_TIMEOUT_MIN ;
    //(random_rand() % (SLOT_TIMEOUT_MAX - SLOT_TIMEOUT_MIN + 1));
  if(WITH_RELAY_CNT()) {
    /* if the relay counter is sent, increment it by the chosen number of
     * slots */
    g.relay_cnt_timeout = g.header.relay_cnt + slot_timeout;
  }
  rtimer_ext_schedule(GLOSSY_CONF_RTIMER_ID, 
                      g.t_timeout + (uint32_t)slot_timeout * g.T_slot_estimated,
                      0,
                      timeout_expired);
}
/*---------------------------------------------------------------------------*/
static inline void
update_t_ref(rtimer_ext_clock_t t_ref, uint8_t relay_cnt)
{
  g.t_ref           = t_ref;
  g.t_ref_updated   = 1;
  g.relay_cnt_t_ref = relay_cnt;
}
/*---------------------------------------------------------------------------*/
static inline void
add_T_slot_measurement(uint32_t T_slot_measured)
{
  if((T_slot_measured > (g.T_slot_estimated - T_SLOT_TOLERANCE)) &&
     (T_slot_measured < (g.T_slot_estimated + T_SLOT_TOLERANCE))) {
    g.T_slot_sum += T_slot_measured;
    g.n_T_slot++;
  }
}
/*---------------------------- Glossy interface -----------------------------*/
void
glossy_start(uint16_t initiator_id,
             uint8_t *payload,
             uint8_t payload_len,
             uint8_t n_tx_max,
             uint8_t with_sync,
             uint8_t with_rf_cal)
{
  GLOSSY_STARTED;

#if GLOSSY_CONF_SETUPTIME_WITH_SYNC
  uint16_t setup_time_start = rtimer_ext_now_lf_hw();
#endif /* GLOSSY_CONF_SETUPTIME_WITH_SYNC */

  /* disable undesired interrupts */
  GLOSSY_DISABLE_INTERRUPTS;

  /* reset the data structure */
  g.active            = 1;
  g.initiator_id      = initiator_id;
  g.payload           = payload;
  g.payload_len       = payload_len;
  g.n_rx              = 0;
  g.n_tx              = 0;
  g.relay_cnt_last_rx = 0;
  g.relay_cnt_last_tx = 0;
  g.t_ref_updated     = 0;
  g.T_slot_sum        = 0;
  g.n_T_slot          = 0;
  g.T_slot_estimated  = 0;

#if GLOSSY_CONF_COLLECT_STATS
  g.stats.last_flood_relay_cnt    = 0;
  g.stats.last_flood_n_rx_started = 0;
  g.stats.last_flood_n_rx_fail    = 0;
  g.stats.last_flood_rssi_sum     = 0;
  g.stats.last_flood_t_to_rx      = 0;
  g.stats.last_flood_duration     = 0;
  g.stats.already_counted         = 0;
#endif /* GLOSSY_CONF_COLLECT_STATS */

  /* prepare the Glossy header, with the information known so far */
  SET_PKT_TYPE(g.header.pkt_type, with_sync, n_tx_max);
  g.header.relay_cnt = 0;

  /* wake-up the radio core */
  rf1a_go_to_idle();

  /* automatically switch to TX at the end of RX */
  rf1a_set_rxoff_mode(RF1A_OFF_MODE_TX);
  /* automatically switch to RX at the end of TX */
  rf1a_set_txoff_mode(RF1A_OFF_MODE_RX);
  /* do not calibrate automatically */
  rf1a_set_calibration_mode(RF1A_CALIBRATION_MODE_MANUAL);

  /* reconfigure lost registers */
  rf1a_reconfig_after_sleep();

  if(with_rf_cal) {
    /* if instructed so, perform a manual calibration */
    rf1a_manual_calibration();
  }

  rf1a_set_header_len_rx(GLOSSY_HEADER_LEN(g.header.pkt_type));
  
  volatile uint16_t timeout;
  if(IS_INITIATOR()) {
    /* Glossy initiator */
    if((g.payload_len + GLOSSY_HEADER_LEN(g.header.pkt_type)) >
       GLOSSY_MAX_PACKET_LEN) {
      /* the initiator must know whether there will be synchronization or
       * not and the packet length may not exceed the max. length */
      glossy_stop();
      return;
    }
#if GLOSSY_CONF_SETUPTIME_WITH_SYNC
    // busy wait for the setup time to pass
    if(with_sync) {
      while((uint16_t)(rtimer_ext_now_lf_hw() - setup_time_start) < GLOSSY_SYNC_SETUP_TICKS);
    }
#endif /* GLOSSY_CONF_SETUPTIME_WITH_SYNC */
    /* start the first transmission */
    g.t_timeout = rtimer_ext_now_hf() + TIMEOUT_EXTRA_TICKS;
    rf1a_start_tx();
#if GLOSSY_CONF_COLLECT_STATS
    g.stats.last_flood_duration = rtimer_ext_now_hf();
#endif /* GLOSSY_CONF_COLLECT_STATS */
    rf1a_write_to_tx_fifo((uint8_t *)&g.header,
                          GLOSSY_HEADER_LEN(g.header.pkt_type),
                          (uint8_t *)g.payload, g.payload_len);
    g.relay_cnt_timeout = 0;
  
  } else {
    /* Glossy receiver */
    rf1a_start_rx();
#if GLOSSY_CONF_COLLECT_STATS
    g.stats.last_flood_duration = rtimer_ext_now_hf();
    /* measure the channel noise (but only if waiting for the schedule */

  #if !GLOSSY_CONF_ALWAYS_SAMPLE_NOISE
    if(with_sync)
  #endif /* GLOSSY_CONF_ALWAYS_SAMPLE_NOISE */
    {
      /* wait after entering RX mode before reading RSSI (see swra114d.pdf)  */
      //__delay_cycles(MCLK_SPEED / 3000);                   /* wait 0.33 ms */
      timeout = 400;                               /* ~400us @13MHz (MSP430) */
      while(!(RF1AIN & BIT1) && timeout) timeout--;   /* wait for RSSI valid */
      if(timeout) {
        g.stats.last_flood_rssi_noise = rf1a_get_rssi();      /* noise floor */
      }
    }
#endif /* GLOSSY_CONF_COLLECT_STATS */
  }
  /* note: RF_RDY bit must be cleared by the radio core before entering LPM
   * after a transition from idle to RX or TX. Either poll the status of the
   * radio core (SNOP strobe) or read the GDOx signal assigned to RF_RDY */
  timeout = 500;                                   /* ~500us @13MHz (MSP430) */
  while((RF1AIN & BIT0) && timeout) timeout--;          /* check GDO0 signal */
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_stop(void)
{
  if(g.active) {
    /* stop the timeout */
    rtimer_ext_stop(GLOSSY_CONF_RTIMER_ID);
    /* flush both RX FIFO and TX FIFO and go to sleep */
    rf1a_flush_rx_fifo();
    rf1a_flush_tx_fifo();
    /* important: if the radio is put into sleep mode, the patable must be 
     * re-configured! see CC1101 datasheet p.33 */
    rf1a_go_to_sleep();
    rf1a_clear_pending_interrupts();

    GLOSSY_RX_STOPPED;
    GLOSSY_TX_STOPPED;
    GLOSSY_STOPPED;
    g.active = 0;

    if(g.t_ref_updated) {
      if(g.n_T_slot > 0) {
        g.t_ref -= (g.relay_cnt_t_ref * g.T_slot_sum) / g.n_T_slot;
      } else {
        g.t_ref -= (uint32_t)g.relay_cnt_t_ref * g.T_slot_estimated;
      }
    }

#if GLOSSY_CONF_COLLECT_STATS
    /* stats */
    g.stats.last_flood_duration = rtimer_ext_now_hf() - 
                                  g.stats.last_flood_duration;
    if(!IS_INITIATOR()) {
      /* only count if not initiator! */
      if(g.stats.last_flood_n_rx_started) {
        /* only count it as flood if at least the start of a packet was
         * detected */
        g.stats.flood_cnt++;
      }
      if(g.n_rx) {
        g.stats.flood_cnt_success++;
      }
    }
#endif /* GLOSSY_CONF_COLLECT_STATS */

    /* re-enable interrupts */
    GLOSSY_ENABLE_INTERRUPTS;
    rtimer_ext_update_enable();
  }

  return g.n_rx;
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_is_active(void)
{
  return g.active;
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_get_rx_cnt(void)
{
  return g.n_rx;
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_get_n_tx(void)
{
  return g.n_tx;
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_get_payload_len(void)
{
  return g.payload_len;
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_is_t_ref_updated(void)
{
  return g.t_ref_updated;
}
/*---------------------------------------------------------------------------*/
rtimer_ext_clock_t
glossy_get_t_ref(void)
{
  return g.t_ref;
}
/*---------------------------------------------------------------------------*/
rtimer_ext_clock_t
glossy_get_t_ref_lf(void)
{
  /* sync HF and LF clocks */
  rtimer_ext_clock_t hf_now, lf_now;
  rtimer_ext_now(&hf_now, &lf_now);
  lf_now = lf_now - 
           (uint32_t)(hf_now - g.t_ref) / (uint32_t)RTIMER_EXT_HF_LF_RATIO;
  return lf_now;
}
/*---------------------------------------------------------------------------*/
#if GLOSSY_CONF_COLLECT_STATS
uint8_t
glossy_get_rx_try_cnt(void)
{
  return g.stats.last_flood_n_rx_started;
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_get_n_crc_ok(void)
{
  return g.stats.pkt_cnt_crcok;
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_get_last_flood_n_rx_fail(void)
{
  return g.stats.last_flood_n_rx_fail;
}
/*---------------------------------------------------------------------------*/
int8_t
glossy_get_snr(void)
{
  /* RSSI values are only valid if at least one packet was received */
  if(g.n_rx == 0 || g.stats.last_flood_rssi_sum == 0 ||
     g.stats.last_flood_rssi_noise == 0) {
    return 0;
  }
  return (int8_t)((g.stats.last_flood_rssi_sum / (int16_t)g.n_rx) -
                  g.stats.last_flood_rssi_noise);
}
/*---------------------------------------------------------------------------*/
int8_t
glossy_get_rssi(void)
{
  /* RSSI values are only valid if at least one packet was received */
  if(g.n_rx == 0 || g.stats.last_flood_rssi_sum == 0) {
    return 0;
  }
  return (int8_t)(g.stats.last_flood_rssi_sum / (int16_t)g.n_rx);
}
/*---------------------------------------------------------------------------*/
uint8_t
glossy_get_relay_cnt(void)
{
  return g.stats.last_flood_relay_cnt;
}
/*---------------------------------------------------------------------------*/
uint16_t
glossy_get_per(void)
{
  if(g.stats.pkt_cnt) {
    return 10000 -
           (uint16_t)((uint64_t)g.stats.pkt_cnt_crcok * 10000 /
           (uint64_t)g.stats.pkt_cnt);
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
uint16_t
glossy_get_fsr(void)
{
  if(g.stats.flood_cnt) {
    return (uint16_t)((uint64_t)g.stats.flood_cnt_success * 10000 /
                      (uint64_t)g.stats.flood_cnt);
  }
  return 10000;
}
/*---------------------------------------------------------------------------*/
uint32_t
glossy_get_n_pkts(void)
{
  return g.stats.pkt_cnt;
}
/*---------------------------------------------------------------------------*/
uint32_t
glossy_get_n_pkts_crcok(void)
{
  return g.stats.pkt_cnt_crcok;
}
/*---------------------------------------------------------------------------*/
uint16_t
glossy_get_n_errors(void)
{
  return g.stats.error_cnt;
}
/*---------------------------------------------------------------------------*/
uint32_t
glossy_get_flood_duration(void)
{
  return (uint32_t)g.stats.last_flood_duration;
}
/*---------------------------------------------------------------------------*/
uint32_t
glossy_get_t_to_first_rx(void)
{
  return (uint32_t)g.stats.last_flood_t_to_rx;
}
/*---------------------------------------------------------------------------*/
int8_t
glossy_get_noise_floor(void)
{
  return (int8_t)g.stats.last_flood_rssi_noise;
}
/*---------------------------------------------------------------------------*/
uint16_t glossy_get_header(void)
{
  return ( (g.header.pkt_type << 8) | (g.header.relay_cnt) );
}
/*---------------------------------------------------------------------------*/
int8_t glossy_get_sync_mode(void)
{
  return WITH_SYNC();
}
/*---------------------------------------------------------------------------*/
void
glossy_reset_stats(void)
{
  g.stats.pkt_cnt = 0;
  g.stats.pkt_cnt_crcok = 0;
  g.stats.flood_cnt = 0;
  g.stats.flood_cnt_success = 0;
  g.stats.error_cnt = 0;
}
#endif /* GLOSSY_CONF_COLLECT_STATS */
/*---------------------- RF1A callback implementation -----------------------*/
void
#if GLOSSY_CONF_USE_RF1A_CALLBACKS
rf1a_cb_rx_started(rtimer_ext_clock_t *timestamp)
#else /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
glossy_rx_started(rtimer_ext_clock_t *timestamp)
#endif /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
{
  GLOSSY_RX_STARTED;

  /* disable timer overflow / update interrupt (required before every RX to
   * make sure that reading from the RX FIFO as well as the RX/TX switching in
   * rf1a_cb_rx_ended is not delayed) */
  rtimer_ext_update_disable();

  g.t_rx_start = *timestamp;
  g.header_ok = 0;
#if GLOSSY_CONF_COLLECT_STATS
  g.stats.already_counted = 0;
  g.stats.pkt_cnt++;
  if(!g.stats.last_flood_n_rx_started) {
    g.stats.last_flood_t_to_rx = *timestamp - g.stats.last_flood_duration;
  }
  g.stats.last_flood_n_rx_started++;
#endif /* GLOSSY_CONF_COLLECT_STATS */

  if(IS_INITIATOR()) {
    /* we are the initiator and we have started a packet reception: stop the
     * timeout */
    rtimer_ext_stop(GLOSSY_CONF_RTIMER_ID);
  }
}
/*---------------------------------------------------------------------------*/
void
#if GLOSSY_CONF_USE_RF1A_CALLBACKS
rf1a_cb_tx_started(rtimer_ext_clock_t *timestamp)
#else /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
glossy_tx_started(rtimer_ext_clock_t *timestamp)
#endif /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
{
  GLOSSY_TX_STARTED;
  g.t_tx_start = *timestamp;

  /* first transmission: estimate the slot length based on the packet length
   * -> Moved to cb_rx_ended
   *    This allows T_slot_estimated to be correctly computed whenever t_ref
   *    is updated. Otherwise, it may be that T_slot_estimated is not updated
   *    in case the flood is terminated before the node does any TX
   *
  if(g.n_tx == 0) {

    g.T_slot_estimated = estimate_T_slot(GLOSSY_HEADER_LEN(g.header.pkt_type) +
                                         g.payload_len);
  }
  */
}
/*---------------------------------------------------------------------------*/
void
#if GLOSSY_CONF_USE_RF1A_CALLBACKS
rf1a_cb_header_received(rtimer_ext_clock_t *timestamp,
                        uint8_t *header,
                        uint8_t packet_len)
#else /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
glossy_header_received(rtimer_ext_clock_t *timestamp,
                       uint8_t *header,
                       uint8_t packet_len)
#endif /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
{
  if(!process_glossy_header(header, packet_len, 0)) {
#if GLOSSY_CONF_COLLECT_STATS
    if(!g.stats.already_counted) {
      g.stats.last_flood_n_rx_fail++;
      g.stats.already_counted = 1;
    }
#endif /* GLOSSY_CONF_COLLECT_STATS */
    /* the header is not ok: interrupt the reception and start a new attempt */
    rf1a_cb_rx_failed(timestamp);
  }
}
/*---------------------------------------------------------------------------*/
void
#if GLOSSY_CONF_USE_RF1A_CALLBACKS
rf1a_cb_rx_ended(rtimer_ext_clock_t *timestamp, uint8_t *pkt, uint8_t pkt_len)
#else /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
glossy_rx_ended(rtimer_ext_clock_t *timestamp, uint8_t *pkt, uint8_t pkt_len)
#endif /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
{
  GLOSSY_RX_STOPPED;
  
  /* enable timer overflow / update interrupt (since we are in an interrupt
   * context here, the timer interrupts will only be handled after this ISR)
   * Note that the RX/TX switching is constant regardless of the runtime of
   * this ISR; it is only necessary to write to the TX queue before the
   * preamble has been sent by the radio module */
  rtimer_ext_update_enable();
  g.t_rx_stop = *timestamp;
#if GLOSSY_CONF_COLLECT_STATS
  g.stats.pkt_cnt_crcok++;
#endif /* GLOSSY_CONF_COLLECT_STATS */

  /* we have received a packet and the CRC is correct, now check the header */
  if(process_glossy_header(pkt, pkt_len, 1)) {
    /* we received a correct packet, and the header has been stored into
     * g.header */
    uint8_t *payload = pkt + GLOSSY_HEADER_LEN(g.header.pkt_type);

    /* store the relay counter corresponding to the first reception */
#if GLOSSY_CONF_COLLECT_STATS
    uint8_t relay_cnt = g.header.relay_cnt;
#endif /* GLOSSY_CONF_COLLECT_STATS */
    /* increment the relay counter */
    g.header.relay_cnt++;

    if((GET_N_TX_MAX(g.header.pkt_type) == 0) ||
       (g.n_tx < GET_N_TX_MAX(g.header.pkt_type))) {
      /* if n_tx_max is either unknown or not yet reached, transmit the
       * packet */
      rf1a_write_to_tx_fifo((uint8_t *)&g.header,
                            GLOSSY_HEADER_LEN(g.header.pkt_type), payload,
                            g.payload_len);
    } else {
      /* otherwise, stop Glossy */
      glossy_stop();
    }

#if GLOSSY_CONF_COLLECT_STATS
    /* stats */
    if(WITH_RELAY_CNT()) {
      /* the relay counter is part of the header */
      if(g.n_rx == 0) {
        g.stats.last_flood_relay_cnt = relay_cnt;
      }
    }
    g.stats.last_flood_rssi_sum += rf1a_get_last_packet_rssi();
#endif /* GLOSSY_CONF_COLLECT_STATS */

    /* increment the reception counter */
    g.n_rx++;
    GLOSSY_FIRST_RX;

    if((!IS_INITIATOR()) && (g.n_rx == 1)) {
      /* we are a receiver and this was our first packet reception: */
      /* store the payload for the application */
      memcpy((uint8_t *)g.payload, payload, g.payload_len);
    }

    if(WITH_SYNC()) {
      /* store the relay counter of this last reception */
      g.relay_cnt_last_rx = g.header.relay_cnt - 1;

      if(g.t_ref_updated == 0) {
        /* t_ref has not been updated yet: update it */
        update_t_ref(g.t_rx_start - NS_TO_RTIMER_EXT_HF(TAU1),
                     g.header.relay_cnt - 1);
        /* Estimate the Glossy slot length */
        g.T_slot_estimated = estimate_T_slot(
            GLOSSY_HEADER_LEN(g.header.pkt_type) + g.payload_len);
      }

      if((g.relay_cnt_last_rx == g.relay_cnt_last_tx + 1) && (g.n_tx > 0)) {
        /* this reception immediately followed a transmission: measure
         * T_slot */
        add_T_slot_measurement(g.t_rx_start - g.t_tx_start -
                               NS_TO_RTIMER_EXT_HF(TAU1));
      }
    }

  } else {
#if GLOSSY_CONF_COLLECT_STATS
    if(!g.stats.already_counted) {
      g.stats.last_flood_n_rx_fail++;
      g.stats.already_counted = 1;
    }
#endif /* GLOSSY_CONF_COLLECT_STATS */
    /* some fields in the header were not correct: discard it */
    rf1a_cb_rx_failed(timestamp);
  }
}
/*---------------------------------------------------------------------------*/
void
#if GLOSSY_CONF_USE_RF1A_CALLBACKS
rf1a_cb_tx_ended(rtimer_ext_clock_t *timestamp)
#else /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
glossy_tx_ended(rtimer_ext_clock_t *timestamp)
#endif /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
{
  GLOSSY_TX_STOPPED;

  g.t_tx_stop = *timestamp;

  if(WITH_SYNC()) {
    /* store the relay counter of this last transmission */
    g.relay_cnt_last_tx = g.header.relay_cnt;

    if(g.t_ref_updated == 0) {
      /* t_ref has not been updated yet: update it */
      update_t_ref(g.t_tx_start, g.header.relay_cnt);
    }
    if((g.relay_cnt_last_tx == g.relay_cnt_last_rx + 1) && (g.n_rx > 0)) {
      /* this transmission immediately followed a reception: measure T_slot */
      add_T_slot_measurement(g.t_tx_start - g.t_rx_start +
                             NS_TO_RTIMER_EXT_HF(TAU1));
    }
  }
  /* increment the transmission counter */
  g.n_tx++;

  if((g.n_tx == GET_N_TX_MAX(g.header.pkt_type)) &&
     (GET_N_TX_MAX(g.header.pkt_type) > 0 || (!IS_INITIATOR()))) {
    /* we have reached N_tx_max and either N_tx_max > 1 or we are a receiver:
     * stop Glossy */
    glossy_stop();
  }
#if GLOSSY_CONF_RETRANSMISSION_TIMEOUT
  else {
    /* radio switches automatically to RX mode */
    if((IS_INITIATOR()) && (g.n_rx == 0)) {
      /* we are the initiator and we still have not received any packet:
       * schedule the timeout */
      schedule_timeout();
    }
  }
#endif /* GLOSSY_CONF_RETRANSMISSION_TIMEOUT */
}
/*---------------------------------------------------------------------------*/
void
#if GLOSSY_CONF_USE_RF1A_CALLBACKS
rf1a_cb_rx_failed(rtimer_ext_clock_t *timestamp)
#else /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
glossy_rx_failed(rtimer_ext_clock_t *timestamp)
#endif /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
{
  GLOSSY_RX_STOPPED;
  /* RX has failed due to invalid CRC or invalid Glossy header */
#if GLOSSY_CONF_COLLECT_STATS
  if(!g.stats.already_counted) {
    g.stats.last_flood_n_rx_fail++;
    g.stats.already_counted = 1;
  }
#endif /* GLOSSY_CONF_COLLECT_STATS */
  /* notify about the failure, flush the RX FIFO and start a new reception
   * attempt */
  //DEBUG_PRINT_VERBOSE("Glossy RX failed, corrupted packet received");

  rtimer_ext_update_enable();
  if(g.active) {
    rf1a_flush_rx_fifo();
    rf1a_start_rx();
  }
}
/*---------------------------------------------------------------------------*/
void
#if GLOSSY_CONF_USE_RF1A_CALLBACKS
rf1a_cb_rx_tx_error(rtimer_ext_clock_t *timestamp)
#else /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
glossy_rx_tx_error(rtimer_ext_clock_t *timestamp)
#endif /* GLOSSY_CONF_USE_RF1A_CALLBACKS */
{
  GLOSSY_RX_STOPPED;
  GLOSSY_TX_STOPPED;

  /* notify about the error (not supposed to occur) */
#if GLOSSY_CONF_COLLECT_STATS
  g.stats.error_cnt++;
#endif /* GLOSSY_CONF_COLLECT_STATS */
  /* in >99% of the cases it's an unexpected falling edge of RFIFG9 */
  //DEBUG_PRINT_VERBOSE("Glossy RX/TX error (interference?)");

  rtimer_ext_update_enable();
  if(g.active) {
    /* if Glossy is still active, flush both RX FIFO and TX FIFO and start a
     * new reception attempt */
    rf1a_flush_rx_fifo();
    rf1a_flush_tx_fifo();
    rf1a_start_rx();
  }
}
/*---------------------------------------------------------------------------*/
