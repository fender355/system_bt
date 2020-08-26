/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains the L2CAP API code
 *
 ******************************************************************************/

#define LOG_TAG "bt_l2cap"

#include <base/logging.h>
#include <base/strings/stringprintf.h>
#include <cstdint>
#include <string>

#include "btm_sec.h"
#include "device/include/controller.h"  // TODO Remove
#include "main/shim/l2c_api.h"
#include "main/shim/shim.h"
#include "osi/include/log.h"
#include "stack/include/l2c_api.h"
#include "stack/l2cap/l2c_int.h"

void btsnd_hcic_enhanced_flush(uint16_t handle,
                               uint8_t packet_type);  // TODO Remove

using base::StringPrintf;

uint16_t L2CA_Register2(uint16_t psm, tL2CAP_APPL_INFO* p_cb_info,
                        bool enable_snoop, tL2CAP_ERTM_INFO* p_ertm_info,
                        uint16_t required_mtu, uint16_t sec_level) {
  auto ret =
      L2CA_Register(psm, p_cb_info, enable_snoop, p_ertm_info, required_mtu);
  BTM_SetSecurityLevel(false, "", 0, sec_level, psm, 0, 0);
  return ret;
}

/*******************************************************************************
 *
 * Function         L2CA_Register
 *
 * Description      Other layers call this function to register for L2CAP
 *                  services.
 *
 * Returns          PSM to use or zero if error. Typically, the PSM returned
 *                  is the same as was passed in, but for an outgoing-only
 *                  connection to a dynamic PSM, a "virtual" PSM is returned
 *                  and should be used in the calls to L2CA_ConnectReq(),
 *                  L2CA_ErtmConnectReq() and L2CA_Deregister()
 *
 ******************************************************************************/
uint16_t L2CA_Register(uint16_t psm, tL2CAP_APPL_INFO* p_cb_info,
                       bool enable_snoop, tL2CAP_ERTM_INFO* p_ertm_info,
                       uint16_t required_mtu) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_Register(psm, p_cb_info, enable_snoop,
                                          p_ertm_info, required_mtu);
  }

  tL2C_RCB* p_rcb;
  uint16_t vpsm = psm;

  L2CAP_TRACE_API("L2CAP - L2CA_Register() called for PSM: 0x%04x", psm);

  /* Verify that the required callback info has been filled in
  **      Note:  Connection callbacks are required but not checked
  **             for here because it is possible to be only a client
  **             or only a server.
  */
  if ((!p_cb_info->pL2CA_ConfigCfm_Cb) || (!p_cb_info->pL2CA_ConfigInd_Cb) ||
      (!p_cb_info->pL2CA_DataInd_Cb) || (!p_cb_info->pL2CA_DisconnectInd_Cb)) {
    L2CAP_TRACE_ERROR("L2CAP - no cb registering PSM: 0x%04x", psm);
    return (0);
  }

  /* Verify PSM is valid */
  if (L2C_INVALID_PSM(psm)) {
    L2CAP_TRACE_ERROR("L2CAP - invalid PSM value, PSM: 0x%04x", psm);
    return (0);
  }

  /* Check if this is a registration for an outgoing-only connection to */
  /* a dynamic PSM. If so, allocate a "virtual" PSM for the app to use. */
  if ((psm >= 0x1001) && (p_cb_info->pL2CA_ConnectInd_Cb == NULL)) {
    for (vpsm = 0x1002; vpsm < 0x8000; vpsm += 2) {
      p_rcb = l2cu_find_rcb_by_psm(vpsm);
      if (p_rcb == NULL) break;
    }

    L2CAP_TRACE_API("L2CA_Register - Real PSM: 0x%04x  Virtual PSM: 0x%04x",
                    psm, vpsm);
  }

  /* If registration block already there, just overwrite it */
  p_rcb = l2cu_find_rcb_by_psm(vpsm);
  if (p_rcb == NULL) {
    p_rcb = l2cu_allocate_rcb(vpsm);
    if (p_rcb == NULL) {
      L2CAP_TRACE_WARNING("L2CAP - no RCB available, PSM: 0x%04x  vPSM: 0x%04x",
                          psm, vpsm);
      return (0);
    }
  }

  p_rcb->log_packets = enable_snoop;
  p_rcb->api = *p_cb_info;
  p_rcb->real_psm = psm;

  return (vpsm);
}

/*******************************************************************************
 *
 * Function         L2CA_Deregister
 *
 * Description      Other layers call this function to de-register for L2CAP
 *                  services.
 *
 * Returns          void
 *
 ******************************************************************************/
void L2CA_Deregister(uint16_t psm) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_Deregister(psm);
  }

  tL2C_RCB* p_rcb;
  tL2C_CCB* p_ccb;
  tL2C_LCB* p_lcb;
  int ii;

  L2CAP_TRACE_API("L2CAP - L2CA_Deregister() called for PSM: 0x%04x", psm);

  p_rcb = l2cu_find_rcb_by_psm(psm);
  if (p_rcb != NULL) {
    p_lcb = &l2cb.lcb_pool[0];
    for (ii = 0; ii < MAX_L2CAP_LINKS; ii++, p_lcb++) {
      if (p_lcb->in_use) {
        p_ccb = p_lcb->ccb_queue.p_first_ccb;
        if ((p_ccb == NULL) || (p_lcb->link_state == LST_DISCONNECTING)) {
          continue;
        }

        if ((p_ccb->in_use) &&
            ((p_ccb->chnl_state == CST_W4_L2CAP_DISCONNECT_RSP) ||
             (p_ccb->chnl_state == CST_W4_L2CA_DISCONNECT_RSP))) {
          continue;
        }

        if (p_ccb->p_rcb == p_rcb) {
          l2c_csm_execute(p_ccb, L2CEVT_L2CA_DISCONNECT_REQ, NULL);
        }
      }
    }
    l2cu_release_rcb(p_rcb);
  } else {
    L2CAP_TRACE_WARNING("L2CAP - PSM: 0x%04x not found for deregistration",
                        psm);
  }
}

/*******************************************************************************
 *
 * Function         L2CA_AllocatePSM
 *
 * Description      Other layers call this function to find an unused PSM for
 *                  L2CAP services.
 *
 * Returns          PSM to use.
 *
 ******************************************************************************/
uint16_t L2CA_AllocatePSM(void) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_AllocatePSM();
  }

  bool done = false;
  uint16_t psm = l2cb.dyn_psm;

  L2CAP_TRACE_API("L2CA_AllocatePSM");
  while (!done) {
    psm += 2;
    if (psm > 0xfeff) {
      psm = 0x1001;
    } else if (psm & 0x0100) {
      /* the upper byte must be even */
      psm += 0x0100;
    }

    /* if psm is in range of reserved BRCM Aware features */
    if ((BRCM_RESERVED_PSM_START <= psm) && (psm <= BRCM_RESERVED_PSM_END))
      continue;

    /* make sure the newlly allocated psm is not used right now */
    if ((l2cu_find_rcb_by_psm(psm)) == NULL) done = true;
  }
  l2cb.dyn_psm = psm;

  return (psm);
}

/*******************************************************************************
 *
 * Function         L2CA_AllocateLePSM
 *
 * Description      To find an unused LE PSM for L2CAP services.
 *
 * Returns          LE_PSM to use if success. Otherwise returns 0.
 *
 ******************************************************************************/
uint16_t L2CA_AllocateLePSM(void) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_AllocateLePSM();
  }

  bool done = false;
  uint16_t psm = l2cb.le_dyn_psm;
  uint16_t count = 0;

  L2CAP_TRACE_API("%s: last psm=%d", __func__, psm);
  while (!done) {
    count++;
    if (count > LE_DYNAMIC_PSM_RANGE) {
      L2CAP_TRACE_ERROR("%s: Out of free BLE PSM", __func__);
      return 0;
    }

    psm++;
    if (psm > LE_DYNAMIC_PSM_END) {
      psm = LE_DYNAMIC_PSM_START;
    }

    if (!l2cb.le_dyn_psm_assigned[psm - LE_DYNAMIC_PSM_START]) {
      /* make sure the newly allocated psm is not used right now */
      if (l2cu_find_ble_rcb_by_psm(psm)) {
        L2CAP_TRACE_WARNING("%s: supposedly-free PSM=%d have allocated rcb!",
                            __func__, psm);
        continue;
      }

      l2cb.le_dyn_psm_assigned[psm - LE_DYNAMIC_PSM_START] = true;
      L2CAP_TRACE_DEBUG("%s: assigned PSM=%d", __func__, psm);
      done = true;
      break;
    }
  }
  l2cb.le_dyn_psm = psm;

  return (psm);
}

/*******************************************************************************
 *
 * Function         L2CA_FreeLePSM
 *
 * Description      Free an assigned LE PSM.
 *
 * Returns          void
 *
 ******************************************************************************/
void L2CA_FreeLePSM(uint16_t psm) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_FreeLePSM(psm);
  }

  L2CAP_TRACE_API("%s: to free psm=%d", __func__, psm);

  if ((psm < LE_DYNAMIC_PSM_START) || (psm > LE_DYNAMIC_PSM_END)) {
    L2CAP_TRACE_ERROR("%s: Invalid PSM=%d value!", __func__, psm);
    return;
  }

  if (!l2cb.le_dyn_psm_assigned[psm - LE_DYNAMIC_PSM_START]) {
    L2CAP_TRACE_WARNING("%s: PSM=%d was not allocated!", __func__, psm);
  }
  l2cb.le_dyn_psm_assigned[psm - LE_DYNAMIC_PSM_START] = false;
}

/*******************************************************************************
 *
 * Function         L2CA_ConnectReq
 *
 * Description      Higher layers call this function to create an L2CAP
 *                  connection. Note that the connection is not established at
 *                  this time, but connection establishment gets started. The
 *                  callback function will be invoked when connection
 *                  establishes or fails.
 *
 * Returns          the CID of the connection, or 0 if it failed to start
 *
 ******************************************************************************/
uint16_t L2CA_ConnectReq(uint16_t psm, const RawAddress& p_bd_addr) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConnectReq(psm, p_bd_addr);
  }

  return L2CA_ErtmConnectReq(psm, p_bd_addr, nullptr);
}

/*******************************************************************************
 *
 * Function         L2CA_ErtmConnectReq
 *
 * Description      Higher layers call this function to create an L2CAP
 *                  connection. Note that the connection is not established at
 *                  this time, but connection establishment gets started. The
 *                  callback function will be invoked when connection
 *                  establishes or fails.
 *
 *  Parameters:       PSM: L2CAP PSM for the connection
 *                    BD address of the peer
 *                   Enhaced retransmission mode configurations

 * Returns          the CID of the connection, or 0 if it failed to start
 *
 ******************************************************************************/
uint16_t L2CA_ErtmConnectReq(uint16_t psm, const RawAddress& p_bd_addr,
                             tL2CAP_ERTM_INFO* p_ertm_info) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ErtmConnectReq(psm, p_bd_addr, p_ertm_info);
  }

  VLOG(1) << __func__ << "BDA " << p_bd_addr
          << StringPrintf(" PSM: 0x%04x allowed:0x%x preferred:%d", psm,
                          (p_ertm_info) ? p_ertm_info->allowed_modes : 0,
                          (p_ertm_info) ? p_ertm_info->preferred_mode : 0);

  /* Fail if we have not established communications with the controller */
  if (!BTM_IsDeviceUp()) {
    LOG(WARNING) << __func__ << ": BTU not ready";
    return 0;
  }
  /* Fail if the PSM is not registered */
  tL2C_RCB* p_rcb = l2cu_find_rcb_by_psm(psm);
  if (p_rcb == nullptr) {
    LOG(WARNING) << __func__ << ": no RCB, PSM=" << loghex(psm);
    return 0;
  }

  /* First, see if we already have a link to the remote */
  /* assume all ERTM l2cap connection is going over BR/EDR for now */
  tL2C_LCB* p_lcb = l2cu_find_lcb_by_bd_addr(p_bd_addr, BT_TRANSPORT_BR_EDR);
  if (p_lcb == nullptr) {
    /* No link. Get an LCB and start link establishment */
    p_lcb = l2cu_allocate_lcb(p_bd_addr, false, BT_TRANSPORT_BR_EDR);
    /* currently use BR/EDR for ERTM mode l2cap connection */
    if (p_lcb == nullptr) {
      LOG(WARNING) << __func__
                   << ": connection not started for PSM=" << loghex(psm)
                   << ", p_lcb=" << p_lcb;
      return 0;
    }
    l2cu_create_conn_br_edr(p_lcb);
  }

  /* Allocate a channel control block */
  tL2C_CCB* p_ccb = l2cu_allocate_ccb(p_lcb, 0);
  if (p_ccb == nullptr) {
    LOG(WARNING) << __func__ << ": no CCB, PSM=" << loghex(psm);
    return 0;
  }

  /* Save registration info */
  p_ccb->p_rcb = p_rcb;

  if (p_ertm_info) {
    p_ccb->ertm_info = *p_ertm_info;

    /* Replace default indicators with the actual default pool */
    if (p_ccb->ertm_info.fcr_rx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.fcr_rx_buf_size = L2CAP_FCR_RX_BUF_SIZE;

    if (p_ccb->ertm_info.fcr_tx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.fcr_tx_buf_size = L2CAP_FCR_TX_BUF_SIZE;

    if (p_ccb->ertm_info.user_rx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.user_rx_buf_size = L2CAP_USER_RX_BUF_SIZE;

    if (p_ccb->ertm_info.user_tx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.user_tx_buf_size = L2CAP_USER_TX_BUF_SIZE;

    p_ccb->max_rx_mtu =
        p_ertm_info->user_rx_buf_size -
        (L2CAP_MIN_OFFSET + L2CAP_SDU_LEN_OFFSET + L2CAP_FCS_LEN);
  }

  /* If link is up, start the L2CAP connection */
  if (p_lcb->link_state == LST_CONNECTED) {
    l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONNECT_REQ, nullptr);
  } else if (p_lcb->link_state == LST_DISCONNECTING) {
    /* If link is disconnecting, save link info to retry after disconnect
     * Possible Race condition when a reconnect occurs
     * on the channel during a disconnect of link. This
     * ccb will be automatically retried after link disconnect
     * arrives
     */
    L2CAP_TRACE_DEBUG("L2CAP API - link disconnecting: RETRY LATER");

    /* Save ccb so it can be started after disconnect is finished */
    p_lcb->p_pending_ccb = p_ccb;
  }

  L2CAP_TRACE_API("L2CAP - L2CA_conn_req(psm: 0x%04x) returned CID: 0x%04x",
                  psm, p_ccb->local_cid);

  /* Return the local CID as our handle */
  return p_ccb->local_cid;
}

/*******************************************************************************
 *
 * Function         L2CA_RegisterLECoc
 *
 * Description      Other layers call this function to register for L2CAP
 *                  Connection Oriented Channel.
 *
 * Returns          PSM to use or zero if error. Typically, the PSM returned
 *                  is the same as was passed in, but for an outgoing-only
 *                  connection to a dynamic PSM, a "virtual" PSM is returned
 *                  and should be used in the calls to L2CA_ConnectLECocReq()
 *                  and L2CA_DeregisterLECoc()
 *
 ******************************************************************************/
uint16_t L2CA_RegisterLECoc(uint16_t psm, tL2CAP_APPL_INFO* p_cb_info) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_RegisterLECoc(psm, p_cb_info);
  }

  L2CAP_TRACE_API("%s called for LE PSM: 0x%04x", __func__, psm);

  /* Verify that the required callback info has been filled in
  **      Note:  Connection callbacks are required but not checked
  **             for here because it is possible to be only a client
  **             or only a server.
  */
  if ((!p_cb_info->pL2CA_DataInd_Cb) || (!p_cb_info->pL2CA_DisconnectInd_Cb)) {
    L2CAP_TRACE_ERROR("%s No cb registering BLE PSM: 0x%04x", __func__, psm);
    return 0;
  }

  /* Verify PSM is valid */
  if (!L2C_IS_VALID_LE_PSM(psm)) {
    L2CAP_TRACE_ERROR("%s Invalid BLE PSM value, PSM: 0x%04x", __func__, psm);
    return 0;
  }

  tL2C_RCB* p_rcb;
  uint16_t vpsm = psm;

  /* Check if this is a registration for an outgoing-only connection to */
  /* a dynamic PSM. If so, allocate a "virtual" PSM for the app to use. */
  if ((psm >= LE_DYNAMIC_PSM_START) &&
      (p_cb_info->pL2CA_ConnectInd_Cb == NULL)) {
    vpsm = L2CA_AllocateLePSM();
    if (vpsm == 0) {
      L2CAP_TRACE_ERROR("%s: Out of free BLE PSM", __func__);
      return 0;
    }

    L2CAP_TRACE_API("%s Real PSM: 0x%04x  Virtual PSM: 0x%04x", __func__, psm,
                    vpsm);
  }

  /* If registration block already there, just overwrite it */
  p_rcb = l2cu_find_ble_rcb_by_psm(vpsm);
  if (p_rcb == NULL) {
    L2CAP_TRACE_API("%s Allocate rcp for Virtual PSM: 0x%04x", __func__, vpsm);
    p_rcb = l2cu_allocate_ble_rcb(vpsm);
    if (p_rcb == NULL) {
      L2CAP_TRACE_WARNING("%s No BLE RCB available, PSM: 0x%04x  vPSM: 0x%04x",
                          __func__, psm, vpsm);
      return 0;
    }
  }

  p_rcb->api = *p_cb_info;
  p_rcb->real_psm = psm;

  return vpsm;
}

/*******************************************************************************
 *
 * Function         L2CA_DeregisterLECoc
 *
 * Description      Other layers call this function to de-register for L2CAP
 *                  Connection Oriented Channel.
 *
 * Returns          void
 *
 ******************************************************************************/
void L2CA_DeregisterLECoc(uint16_t psm) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_DeregisterLECoc(psm);
  }

  L2CAP_TRACE_API("%s called for PSM: 0x%04x", __func__, psm);

  tL2C_RCB* p_rcb = l2cu_find_ble_rcb_by_psm(psm);
  if (p_rcb == NULL) {
    L2CAP_TRACE_WARNING("%s PSM: 0x%04x not found for deregistration", __func__,
                        psm);
    return;
  }

  tL2C_LCB* p_lcb = &l2cb.lcb_pool[0];
  for (int i = 0; i < MAX_L2CAP_LINKS; i++, p_lcb++) {
    if (!p_lcb->in_use || p_lcb->transport != BT_TRANSPORT_LE) continue;

    tL2C_CCB* p_ccb = p_lcb->ccb_queue.p_first_ccb;
    if ((p_ccb == NULL) || (p_lcb->link_state == LST_DISCONNECTING)) continue;

    if (p_ccb->in_use && (p_ccb->chnl_state == CST_W4_L2CAP_DISCONNECT_RSP ||
                          p_ccb->chnl_state == CST_W4_L2CA_DISCONNECT_RSP))
      continue;

    if (p_ccb->p_rcb == p_rcb)
      l2c_csm_execute(p_ccb, L2CEVT_L2CA_DISCONNECT_REQ, NULL);
  }

  l2cu_release_ble_rcb(p_rcb);
}

/*******************************************************************************
 *
 * Function         L2CA_ConnectLECocReq
 *
 * Description      Higher layers call this function to create an L2CAP
 *                  connection. Note that the connection is not established at
 *                  this time, but connection establishment gets started. The
 *                  callback function will be invoked when connection
 *                  establishes or fails.
 *
 *  Parameters:     PSM: L2CAP PSM for the connection
 *                  BD address of the peer
 *                  Local Coc configurations

 * Returns          the CID of the connection, or 0 if it failed to start
 *
 ******************************************************************************/
uint16_t L2CA_ConnectLECocReq(uint16_t psm, const RawAddress& p_bd_addr,
                              tL2CAP_LE_CFG_INFO* p_cfg) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConnectLECocReq(psm, p_bd_addr, p_cfg);
  }

  VLOG(1) << __func__ << " BDA: " << p_bd_addr
          << StringPrintf(" PSM: 0x%04x", psm);

  /* Fail if we have not established communications with the controller */
  if (!BTM_IsDeviceUp()) {
    L2CAP_TRACE_WARNING("%s BTU not ready", __func__);
    return 0;
  }

  /* Fail if the PSM is not registered */
  tL2C_RCB* p_rcb = l2cu_find_ble_rcb_by_psm(psm);
  if (p_rcb == NULL) {
    L2CAP_TRACE_WARNING("%s No BLE RCB, PSM: 0x%04x", __func__, psm);
    return 0;
  }

  /* First, see if we already have a le link to the remote */
  tL2C_LCB* p_lcb = l2cu_find_lcb_by_bd_addr(p_bd_addr, BT_TRANSPORT_LE);
  if (p_lcb == NULL) {
    /* No link. Get an LCB and start link establishment */
    p_lcb = l2cu_allocate_lcb(p_bd_addr, false, BT_TRANSPORT_LE);
    if ((p_lcb == NULL)
        /* currently use BR/EDR for ERTM mode l2cap connection */
        || (!l2cu_create_conn_le(p_lcb))) {
      L2CAP_TRACE_WARNING("%s conn not started for PSM: 0x%04x  p_lcb: 0x%08x",
                          __func__, psm, p_lcb);
      return 0;
    }
  }

  /* Allocate a channel control block */
  tL2C_CCB* p_ccb = l2cu_allocate_ccb(p_lcb, 0);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("%s no CCB, PSM: 0x%04x", __func__, psm);
    return 0;
  }

  /* Save registration info */
  p_ccb->p_rcb = p_rcb;

  /* Save the configuration */
  if (p_cfg) {
    p_ccb->local_conn_cfg = *p_cfg;
    p_ccb->remote_credit_count = p_cfg->credits;
  }

  /* If link is up, start the L2CAP connection */
  if (p_lcb->link_state == LST_CONNECTED) {
    if (p_ccb->p_lcb->transport == BT_TRANSPORT_LE) {
      L2CAP_TRACE_DEBUG("%s LE Link is up", __func__);
      l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONNECT_REQ, NULL);
    }
  }

  /* If link is disconnecting, save link info to retry after disconnect
   * Possible Race condition when a reconnect occurs
   * on the channel during a disconnect of link. This
   * ccb will be automatically retried after link disconnect
   * arrives
   */
  else if (p_lcb->link_state == LST_DISCONNECTING) {
    L2CAP_TRACE_DEBUG("%s link disconnecting: RETRY LATER", __func__);

    /* Save ccb so it can be started after disconnect is finished */
    p_lcb->p_pending_ccb = p_ccb;
  }

  L2CAP_TRACE_API("%s(psm: 0x%04x) returned CID: 0x%04x", __func__, psm,
                  p_ccb->local_cid);

  /* Return the local CID as our handle */
  return p_ccb->local_cid;
}

/*******************************************************************************
 *
 * Function         L2CA_ConnectLECocRsp
 *
 * Description      Higher layers call this function to accept an incoming
 *                  L2CAP COC connection, for which they had gotten an connect
 *                  indication callback.
 *
 * Returns          true for success, false for failure
 *
 ******************************************************************************/
bool L2CA_ConnectLECocRsp(const RawAddress& p_bd_addr, uint8_t id,
                          uint16_t lcid, uint16_t result, uint16_t status,
                          tL2CAP_LE_CFG_INFO* p_cfg) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConnectLECocRsp(p_bd_addr, id, lcid, result,
                                                 status, p_cfg);
  }

  VLOG(1) << __func__ << " BDA: " << p_bd_addr
          << StringPrintf(" CID: 0x%04x Result: %d Status: %d", lcid, result,
                          status);

  /* First, find the link control block */
  tL2C_LCB* p_lcb = l2cu_find_lcb_by_bd_addr(p_bd_addr, BT_TRANSPORT_LE);
  if (p_lcb == NULL) {
    /* No link. Get an LCB and start link establishment */
    L2CAP_TRACE_WARNING("%s no LCB", __func__);
    return false;
  }

  /* Now, find the channel control block */
  tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("%s no CCB", __func__);
    return false;
  }

  /* The IDs must match */
  if (p_ccb->remote_id != id) {
    L2CAP_TRACE_WARNING("%s bad id. Expected: %d  Got: %d", __func__,
                        p_ccb->remote_id, id);
    return false;
  }

  if (p_cfg) {
    p_ccb->local_conn_cfg = *p_cfg;
    p_ccb->remote_credit_count = p_cfg->credits;
  }

  if (result == L2CAP_CONN_OK)
    l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONNECT_RSP, NULL);
  else {
    tL2C_CONN_INFO conn_info;
    conn_info.bd_addr = p_bd_addr;
    conn_info.l2cap_result = result;
    conn_info.l2cap_status = status;
    l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONNECT_RSP_NEG, &conn_info);
  }

  return true;
}

/*******************************************************************************
 *
 *  Function         L2CA_GetPeerLECocConfig
 *
 *  Description      Get a peers configuration for LE Connection Oriented
 *                   Channel.
 *
 *  Parameters:      local channel id
 *                   Pointers to peers configuration storage area
 *
 *  Return value:    true if peer is connected
 *
 ******************************************************************************/
bool L2CA_GetPeerLECocConfig(uint16_t lcid, tL2CAP_LE_CFG_INFO* peer_cfg) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_GetPeerLECocConfig(lcid, peer_cfg);
  }

  L2CAP_TRACE_API("%s CID: 0x%04x", __func__, lcid);

  tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(NULL, lcid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_ERROR("%s No CCB for CID:0x%04x", __func__, lcid);
    return false;
  }

  if (peer_cfg != NULL)
    memcpy(peer_cfg, &p_ccb->peer_conn_cfg, sizeof(tL2CAP_LE_CFG_INFO));

  return true;
}

/*******************************************************************************
 *
 * Function         L2CA_ConnectRsp
 *
 * Description      Higher layers call this function to accept an incoming
 *                  L2CAP connection, for which they had gotten an connect
 *                  indication callback.
 *
 * Returns          true for success, false for failure
 *
 ******************************************************************************/
bool L2CA_ConnectRsp(const RawAddress& p_bd_addr, uint8_t id, uint16_t lcid,
                     uint16_t result, uint16_t status) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConnectRsp(p_bd_addr, id, lcid, result,
                                            status);
  }

  return L2CA_ErtmConnectRsp(p_bd_addr, id, lcid, result, status, NULL);
}

/*******************************************************************************
 *
 * Function         L2CA_ErtmConnectRsp
 *
 * Description      Higher layers call this function to accept an incoming
 *                  L2CAP connection, for which they had gotten an connect
 *                  indication callback.
 *
 * Returns          true for success, false for failure
 *
 ******************************************************************************/
bool L2CA_ErtmConnectRsp(const RawAddress& p_bd_addr, uint8_t id, uint16_t lcid,
                         uint16_t result, uint16_t status,
                         tL2CAP_ERTM_INFO* p_ertm_info) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ErtmConnectRsp(p_bd_addr, id, lcid, result,
                                                status, p_ertm_info);
  }

  tL2C_LCB* p_lcb;
  tL2C_CCB* p_ccb;

  VLOG(1) << __func__ << " BDA: " << p_bd_addr
          << StringPrintf(" CID:0x%04x  Result:%d  Status:%d", lcid, result,
                          status);

  /* First, find the link control block */
  p_lcb = l2cu_find_lcb_by_bd_addr(p_bd_addr, BT_TRANSPORT_BR_EDR);
  if (p_lcb == NULL) {
    /* No link. Get an LCB and start link establishment */
    L2CAP_TRACE_WARNING("L2CAP - no LCB for L2CA_conn_rsp");
    return (false);
  }

  /* Now, find the channel control block */
  p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_conn_rsp");
    return (false);
  }

  /* The IDs must match */
  if (p_ccb->remote_id != id) {
    L2CAP_TRACE_WARNING("L2CAP - bad id in L2CA_conn_rsp. Exp: %d  Got: %d",
                        p_ccb->remote_id, id);
    return (false);
  }

  if (p_ertm_info) {
    p_ccb->ertm_info = *p_ertm_info;

    /* Replace default indicators with the actual default pool */
    if (p_ccb->ertm_info.fcr_rx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.fcr_rx_buf_size = L2CAP_FCR_RX_BUF_SIZE;

    if (p_ccb->ertm_info.fcr_tx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.fcr_tx_buf_size = L2CAP_FCR_TX_BUF_SIZE;

    if (p_ccb->ertm_info.user_rx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.user_rx_buf_size = L2CAP_USER_RX_BUF_SIZE;

    if (p_ccb->ertm_info.user_tx_buf_size == L2CAP_INVALID_ERM_BUF_SIZE)
      p_ccb->ertm_info.user_tx_buf_size = L2CAP_USER_TX_BUF_SIZE;

    p_ccb->max_rx_mtu =
        p_ertm_info->user_rx_buf_size -
        (L2CAP_MIN_OFFSET + L2CAP_SDU_LEN_OFFSET + L2CAP_FCS_LEN);
  }

  if (result == L2CAP_CONN_OK) {
    l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONNECT_RSP, NULL);
  } else {
    tL2C_CONN_INFO conn_info;

    conn_info.l2cap_result = result;
    conn_info.l2cap_status = status;

    if (result == L2CAP_CONN_PENDING)
      l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONNECT_RSP, &conn_info);
    else
      l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONNECT_RSP_NEG, &conn_info);
  }

  return (true);
}

/*******************************************************************************
 *
 * Function         L2CA_ConfigReq
 *
 * Description      Higher layers call this function to send configuration.
 *
 *                  Note:  The FCR options of p_cfg are not used.
 *
 * Returns          true if configuration sent, else false
 *
 ******************************************************************************/
bool L2CA_ConfigReq(uint16_t cid, tL2CAP_CFG_INFO* p_cfg) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConfigReq(cid, p_cfg);
  }

  tL2C_CCB* p_ccb;

  L2CAP_TRACE_API(
      "L2CA_ConfigReq()  CID 0x%04x: fcr_present:%d (mode %d) mtu_present:%d "
      "(%d)",
      cid, p_cfg->fcr_present, p_cfg->fcr.mode, p_cfg->mtu_present, p_cfg->mtu);

  /* Find the channel control block. We don't know the link it is on. */
  p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_cfg_req, CID: %d", cid);
    return (false);
  }

  /* We need to have at least one mode type common with the peer */
  if (!l2c_fcr_adj_our_req_options(p_ccb, p_cfg)) return (false);

  /* Don't adjust FCR options if not used */
  if ((!p_cfg->fcr_present) || (p_cfg->fcr.mode == L2CAP_FCR_BASIC_MODE)) {
    /* FCR and FCS options are not used in basic mode */
    p_cfg->fcs_present = false;
    p_cfg->ext_flow_spec_present = false;

    if ((p_cfg->mtu_present) && (p_cfg->mtu > L2CAP_MTU_SIZE)) {
      L2CAP_TRACE_WARNING("L2CAP - adjust MTU: %u too large", p_cfg->mtu);
      p_cfg->mtu = L2CAP_MTU_SIZE;
    }
  }

  /* Save the adjusted configuration in case it needs to be used for
   * renegotiation */
  p_ccb->our_cfg = *p_cfg;

  l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONFIG_REQ, p_cfg);

  return (true);
}

/*******************************************************************************
 *
 * Function         L2CA_ConfigRsp
 *
 * Description      Higher layers call this function to send a configuration
 *                  response.
 *
 * Returns          true if configuration response sent, else false
 *
 ******************************************************************************/
bool L2CA_ConfigRsp(uint16_t cid, tL2CAP_CFG_INFO* p_cfg) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConfigRsp(cid, p_cfg);
  }

  tL2C_CCB* p_ccb;

  L2CAP_TRACE_API(
      "L2CA_ConfigRsp()  CID: 0x%04x  Result: %d MTU present:%d Flush TO:%d "
      "FCR:%d FCS:%d",
      cid, p_cfg->result, p_cfg->mtu_present, p_cfg->flush_to_present,
      p_cfg->fcr_present, p_cfg->fcs_present);

  /* Find the channel control block. We don't know the link it is on. */
  p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_cfg_rsp, CID: %d", cid);
    return (false);
  }

  if ((p_cfg->result == L2CAP_CFG_OK) || (p_cfg->result == L2CAP_CFG_PENDING))
    l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONFIG_RSP, p_cfg);
  else {
    p_cfg->fcr_present =
        false; /* FCR options already negotiated before this point */

    /* Clear out any cached options that are being returned as an error
     * (excluding FCR) */
    if (p_cfg->mtu_present) p_ccb->peer_cfg.mtu_present = false;
    if (p_cfg->flush_to_present) p_ccb->peer_cfg.flush_to_present = false;
    if (p_cfg->qos_present) p_ccb->peer_cfg.qos_present = false;

    l2c_csm_execute(p_ccb, L2CEVT_L2CA_CONFIG_RSP_NEG, p_cfg);
  }

  return (true);
}

/*******************************************************************************
 *
 * Function         L2CA_DisconnectReq
 *
 * Description      Higher layers call this function to disconnect a channel.
 *
 * Returns          true if disconnect sent, else false
 *
 ******************************************************************************/
bool L2CA_DisconnectReq(uint16_t cid) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_DisconnectReq(cid);
  }

  tL2C_CCB* p_ccb;

  L2CAP_TRACE_API("L2CA_DisconnectReq()  CID: 0x%04x", cid);

  /* Find the channel control block. We don't know the link it is on. */
  p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_disc_req, CID: %d", cid);
    return (false);
  }

  l2c_csm_execute(p_ccb, L2CEVT_L2CA_DISCONNECT_REQ, NULL);

  return (true);
}

/*******************************************************************************
 *
 * Function         L2CA_DisconnectRsp
 *
 * Description      Higher layers call this function to acknowledge the
 *                  disconnection of a channel.
 *
 * Returns          void
 *
 ******************************************************************************/
bool L2CA_DisconnectRsp(uint16_t cid) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_DisconnectRsp(cid);
  }

  tL2C_CCB* p_ccb;

  L2CAP_TRACE_API("L2CA_DisconnectRsp()  CID: 0x%04x", cid);

  /* Find the channel control block. We don't know the link it is on. */
  p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_disc_rsp, CID: %d", cid);
    return (false);
  }

  l2c_csm_execute(p_ccb, L2CEVT_L2CA_DISCONNECT_RSP, NULL);

  return (true);
}

bool L2CA_GetRemoteCid(uint16_t lcid, uint16_t* rcid) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_GetRemoteCid(lcid, rcid);
  }

  tL2C_CCB* control_block = l2cu_find_ccb_by_cid(NULL, lcid);
  if (!control_block) return false;

  if (rcid) *rcid = control_block->remote_cid;

  return true;
}

/*******************************************************************************
 *
 * Function         L2CA_SetIdleTimeout
 *
 * Description      Higher layers call this function to set the idle timeout for
 *                  a connection, or for all future connections. The "idle
 *                  timeout" is the amount of time that a connection can remain
 *                  up with no L2CAP channels on it. A timeout of zero means
 *                  that the connection will be torn down immediately when the
 *                  last channel is removed. A timeout of 0xFFFF means no
 *                  timeout. Values are in seconds.
 *
 * Returns          true if command succeeded, false if failed
 *
 * NOTE             This timeout takes effect after at least 1 channel has been
 *                  established and removed. L2CAP maintains its own timer from
 *                  whan a connection is established till the first channel is
 *                  set up.
 ******************************************************************************/
bool L2CA_SetIdleTimeout(uint16_t cid, uint16_t timeout, bool is_global) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SetIdleTimeout(cid, timeout, is_global);
  }

  tL2C_CCB* p_ccb;
  tL2C_LCB* p_lcb;

  if (is_global) {
    l2cb.idle_timeout = timeout;
  } else {
    /* Find the channel control block. We don't know the link it is on. */
    p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
    if (p_ccb == NULL) {
      L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_SetIdleTimeout, CID: %d",
                          cid);
      return (false);
    }

    p_lcb = p_ccb->p_lcb;

    if ((p_lcb) && (p_lcb->in_use) && (p_lcb->link_state == LST_CONNECTED))
      p_lcb->idle_timeout = timeout;
    else
      return (false);
  }

  return (true);
}

/*******************************************************************************
 *
 * Function         L2CA_SetIdleTimeoutByBdAddr
 *
 * Description      Higher layers call this function to set the idle timeout for
 *                  a connection. The "idle timeout" is the amount of time that
 *                  a connection can remain up with no L2CAP channels on it.
 *                  A timeout of zero means that the connection will be torn
 *                  down immediately when the last channel is removed.
 *                  A timeout of 0xFFFF means no timeout. Values are in seconds.
 *                  A bd_addr is the remote BD address. If bd_addr =
 *                  RawAddress::kAny, then the idle timeouts for all active
 *                  l2cap links will be changed.
 *
 * Returns          true if command succeeded, false if failed
 *
 * NOTE             This timeout applies to all logical channels active on the
 *                  ACL link.
 ******************************************************************************/
bool L2CA_SetIdleTimeoutByBdAddr(const RawAddress& bd_addr, uint16_t timeout,
                                 tBT_TRANSPORT transport) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SetIdleTimeoutByBdAddr(bd_addr, timeout,
                                                        transport);
  }

  tL2C_LCB* p_lcb;

  if (RawAddress::kAny != bd_addr) {
    p_lcb = l2cu_find_lcb_by_bd_addr(bd_addr, transport);
    if ((p_lcb) && (p_lcb->in_use) && (p_lcb->link_state == LST_CONNECTED)) {
      p_lcb->idle_timeout = timeout;

      if (!p_lcb->ccb_queue.p_first_ccb) l2cu_no_dynamic_ccbs(p_lcb);
    } else
      return false;
  } else {
    int xx;
    tL2C_LCB* p_lcb = &l2cb.lcb_pool[0];

    for (xx = 0; xx < MAX_L2CAP_LINKS; xx++, p_lcb++) {
      if ((p_lcb->in_use) && (p_lcb->link_state == LST_CONNECTED)) {
        p_lcb->idle_timeout = timeout;

        if (!p_lcb->ccb_queue.p_first_ccb) l2cu_no_dynamic_ccbs(p_lcb);
      }
    }
  }

  return true;
}

/*******************************************************************************
 *
 * Function         L2CA_SetTraceLevel
 *
 * Description      This function sets the trace level for L2CAP. If called with
 *                  a value of 0xFF, it simply reads the current trace level.
 *
 * Returns          the new (current) trace level
 *
 ******************************************************************************/
uint8_t L2CA_SetTraceLevel(uint8_t new_level) {
  if (new_level != 0xFF) l2cb.l2cap_trace_level = new_level;

  return (l2cb.l2cap_trace_level);
}

/*******************************************************************************
 *
 * Function         L2CA_SetAclPriority
 *
 * Description      Sets the transmission priority for a channel.
 *                  (For initial implementation only two values are valid.
 *                  L2CAP_PRIORITY_NORMAL and L2CAP_PRIORITY_HIGH).
 *
 * Returns          true if a valid channel, else false
 *
 ******************************************************************************/
bool L2CA_SetAclPriority(const RawAddress& bd_addr, uint8_t priority) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SetAclPriority(bd_addr, priority);
  }

  VLOG(1) << __func__ << " BDA: " << bd_addr
          << ", priority: " << std::to_string(priority);
  return (l2cu_set_acl_priority(bd_addr, priority, false));
}

/*******************************************************************************
 *
 * Function         L2CA_SetTxPriority
 *
 * Description      Sets the transmission priority for a channel.
 *
 * Returns          true if a valid channel, else false
 *
 ******************************************************************************/
bool L2CA_SetTxPriority(uint16_t cid, tL2CAP_CHNL_PRIORITY priority) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SetTxPriority(cid, priority);
  }

  tL2C_CCB* p_ccb;

  L2CAP_TRACE_API("L2CA_SetTxPriority()  CID: 0x%04x, priority:%d", cid,
                  priority);

  /* Find the channel control block. We don't know the link it is on. */
  p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_SetTxPriority, CID: %d", cid);
    return (false);
  }

  /* it will update the order of CCB in LCB by priority and update round robin
   * service variables */
  l2cu_change_pri_ccb(p_ccb, priority);

  return (true);
}

/*******************************************************************************
 *
 * Function         L2CA_SetFlushTimeout
 *
 * Description      This function set the automatic flush time out in Baseband
 *                  for ACL-U packets.
 *                  BdAddr : the remote BD address of ACL link. If it is
 *                          BT_DB_ANY then the flush time out will be applied to
 *                          all ACL links.
 *                  FlushTimeout: flush time out in ms
 *                           0x0000 : No automatic flush
 *                           0x0001 : No retransmission
 *                           0x0002 - 0x0500 : flush time out in ms
 *                           0xffff : DEPRECATED No automatic flush (0x0000)
 *                           >0x0500 : Illegal value
 *
 * Returns          true if command succeeded, false if failed
 *
 * NOTE             This flush timeout applies to all logical channels active on
 *                  the ACL link.
 ******************************************************************************/
inline uint32_t ConvertMillisecondsToBasebandSlots(uint32_t milliseconds) {
  return ((milliseconds * 8) + 3) / 5;
}

bool L2CA_SetFlushTimeout(const RawAddress& bd_addr,
                          const uint16_t flush_timeout_in_ms) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SetFlushTimeout(bd_addr, flush_timeout_in_ms);
  }

  if (flush_timeout_in_ms == L2CAP_NO_AUTOMATIC_FLUSH) {
    LOG_WARN(
        "%s Parameter deprecated for no automatic flush; please use 0x0000",
        __func__);
  } else if (ConvertMillisecondsToBasebandSlots(flush_timeout_in_ms) >
             HCI_MAX_AUTOMATIC_FLUSH_TIMEOUT) {
    LOG_WARN(
        "%s Unable to set flush timeout larger then controller capability:%dms",
        __func__, flush_timeout_in_ms);
    return false;
  }

  uint16_t flush_timeout_in_slots = 0;

  /* no automatic flush (infinite timeout) */
  if (flush_timeout_in_ms == 0x0000 ||
      flush_timeout_in_ms == L2CAP_NO_AUTOMATIC_FLUSH) {
    flush_timeout_in_slots = 0x0;
  } else if (flush_timeout_in_ms == L2CAP_NO_RETRANSMISSION) {
    /* no retransmission */
    /* not mandatory range for controller */
    /* Packet is flushed before getting any ACK/NACK */
    /* To do this, flush timeout should be 1 baseband slot */
    flush_timeout_in_slots = 0x0001;
  } else {
    flush_timeout_in_slots =
        ConvertMillisecondsToBasebandSlots(flush_timeout_in_ms);
  }

  if (RawAddress::kAny != bd_addr) {
    tL2C_LCB* p_lcb = l2cu_find_lcb_by_bd_addr(bd_addr, BT_TRANSPORT_BR_EDR);

    if ((p_lcb) && (p_lcb->in_use) && (p_lcb->link_state == LST_CONNECTED)) {
      if (p_lcb->LinkFlushTimeout() != flush_timeout_in_ms) {
        p_lcb->SetLinkFlushTimeout(flush_timeout_in_ms);
        acl_write_automatic_flush_timeout(bd_addr, flush_timeout_in_slots);
      }
    } else {
      LOG(WARNING) << __func__ << " No lcb for bd_addr " << bd_addr;
      return false;
    }
  } else {
    tL2C_LCB* p_lcb = &l2cb.lcb_pool[0];
    for (int xx = 0; xx < MAX_L2CAP_LINKS; xx++, p_lcb++) {
      if ((p_lcb->in_use) && (p_lcb->link_state == LST_CONNECTED)) {
        if (p_lcb->LinkFlushTimeout() != flush_timeout_in_ms) {
          p_lcb->SetLinkFlushTimeout(flush_timeout_in_ms);
          acl_write_automatic_flush_timeout(p_lcb->remote_bd_addr,
                                            flush_timeout_in_slots);
        }
      }
    }
  }
  return true;
}

/*******************************************************************************
 *
 *  Function         L2CA_GetPeerFeatures
 *
 *  Description      Get a peers features and fixed channel map
 *
 *  Parameters:      BD address of the peer
 *                   Pointers to features and channel mask storage area
 *
 *  Return value:    true if peer is connected
 *
 ******************************************************************************/
bool L2CA_GetPeerFeatures(const RawAddress& bd_addr, uint32_t* p_ext_feat,
                          uint8_t* p_chnl_mask) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_GetPeerFeatures(bd_addr, p_ext_feat,
                                                 p_chnl_mask);
  }

  tL2C_LCB* p_lcb;

  /* We must already have a link to the remote */
  p_lcb = l2cu_find_lcb_by_bd_addr(bd_addr, BT_TRANSPORT_BR_EDR);
  if (p_lcb == NULL) {
    LOG(WARNING) << __func__ << " No BDA: " << bd_addr;
    return false;
  }

  VLOG(1) << __func__ << " BDA: " << bd_addr
          << StringPrintf(" ExtFea: 0x%08x Chnl_Mask[0]: 0x%02x",
                          p_lcb->peer_ext_fea, p_lcb->peer_chnl_mask[0]);

  *p_ext_feat = p_lcb->peer_ext_fea;

  memcpy(p_chnl_mask, p_lcb->peer_chnl_mask, L2CAP_FIXED_CHNL_ARRAY_SIZE);

  return true;
}

/*******************************************************************************
 *
 *  Function        L2CA_RegisterFixedChannel
 *
 *  Description     Register a fixed channel.
 *
 *  Parameters:     Fixed Channel #
 *                  Channel Callbacks and config
 *
 *  Return value:   -
 *
 ******************************************************************************/
bool L2CA_RegisterFixedChannel(uint16_t fixed_cid,
                               tL2CAP_FIXED_CHNL_REG* p_freg) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_RegisterFixedChannel(fixed_cid, p_freg);
  }

  if ((fixed_cid < L2CAP_FIRST_FIXED_CHNL) ||
      (fixed_cid > L2CAP_LAST_FIXED_CHNL)) {
    L2CAP_TRACE_ERROR("L2CA_RegisterFixedChannel()  Invalid CID: 0x%04x",
                      fixed_cid);

    return (false);
  }

  l2cb.fixed_reg[fixed_cid - L2CAP_FIRST_FIXED_CHNL] = *p_freg;
  return (true);
}

/*******************************************************************************
 *
 *  Function        L2CA_ConnectFixedChnl
 *
 *  Description     Connect an fixed signalling channel to a remote device.
 *
 *  Parameters:     Fixed CID
 *                  BD Address of remote
 *
 *  Return value:   true if connection started
 *
 ******************************************************************************/
bool L2CA_ConnectFixedChnl(uint16_t fixed_cid, const RawAddress& rem_bda) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConnectFixedChnl(fixed_cid, rem_bda);
  }
  uint8_t phy = controller_get_interface()->get_le_all_initiating_phys();
  return L2CA_ConnectFixedChnl(fixed_cid, rem_bda, phy);
}

bool L2CA_ConnectFixedChnl(uint16_t fixed_cid, const RawAddress& rem_bda,
                           uint8_t initiating_phys) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_ConnectFixedChnl(fixed_cid, rem_bda,
                                                  initiating_phys);
  }

  tL2C_LCB* p_lcb;
  tBT_TRANSPORT transport = BT_TRANSPORT_BR_EDR;

  VLOG(1) << __func__ << " BDA: " << rem_bda
          << StringPrintf("CID: 0x%04x ", fixed_cid);

  // Check CID is valid and registered
  if ((fixed_cid < L2CAP_FIRST_FIXED_CHNL) ||
      (fixed_cid > L2CAP_LAST_FIXED_CHNL) ||
      (l2cb.fixed_reg[fixed_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedData_Cb ==
       NULL)) {
    L2CAP_TRACE_ERROR("%s() Invalid CID: 0x%04x", __func__, fixed_cid);
    return (false);
  }

  // Fail if BT is not yet up
  if (!BTM_IsDeviceUp()) {
    L2CAP_TRACE_WARNING("%s(0x%04x) - BTU not ready", __func__, fixed_cid);
    return (false);
  }

  if (fixed_cid >= L2CAP_ATT_CID && fixed_cid <= L2CAP_SMP_CID)
    transport = BT_TRANSPORT_LE;

  tL2C_BLE_FIXED_CHNLS_MASK peer_channel_mask;

  // If we already have a link to the remote, check if it supports that CID
  p_lcb = l2cu_find_lcb_by_bd_addr(rem_bda, transport);
  if (p_lcb != NULL) {
    // Fixed channels are mandatory on LE transports so ignore the received
    // channel mask and use the locally cached LE channel mask.

    if (transport == BT_TRANSPORT_LE)
      peer_channel_mask = l2cb.l2c_ble_fixed_chnls_mask;
    else
      peer_channel_mask = p_lcb->peer_chnl_mask[0];

    // Check for supported channel
    if (!(peer_channel_mask & (1 << fixed_cid))) {
      VLOG(2) << __func__ << " BDA " << rem_bda
              << StringPrintf(" CID:0x%04x not supported", fixed_cid);
      return false;
    }

    // Get a CCB and link the lcb to it
    if (!l2cu_initialize_fixed_ccb(p_lcb, fixed_cid)) {
      L2CAP_TRACE_WARNING("%s(0x%04x) - LCB but no CCB", __func__, fixed_cid);
      return false;
    }

    // racing with disconnecting, queue the connection request
    if (p_lcb->link_state == LST_DISCONNECTING) {
      L2CAP_TRACE_DEBUG("$s() - link disconnecting: RETRY LATER", __func__);
      /* Save ccb so it can be started after disconnect is finished */
      p_lcb->p_pending_ccb =
          p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL];
      return true;
    }

    (*l2cb.fixed_reg[fixed_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedConn_Cb)(
        fixed_cid, p_lcb->remote_bd_addr, true, 0, p_lcb->transport);
    return true;
  }

  // No link. Get an LCB and start link establishment
  p_lcb = l2cu_allocate_lcb(rem_bda, false, transport);
  if (p_lcb == NULL) {
    L2CAP_TRACE_WARNING("%s(0x%04x) - no LCB", __func__, fixed_cid);
    return false;
  }

  // Get a CCB and link the lcb to it
  if (!l2cu_initialize_fixed_ccb(p_lcb, fixed_cid)) {
    p_lcb->SetDisconnectReason(L2CAP_CONN_NO_RESOURCES);
    L2CAP_TRACE_WARNING("%s(0x%04x) - no CCB", __func__, fixed_cid);
    l2cu_release_lcb(p_lcb);
    return false;
  }

  if (transport == BT_TRANSPORT_LE) {
    bool ret = l2cu_create_conn_le(p_lcb, initiating_phys);
    if (!ret) {
      L2CAP_TRACE_WARNING("%s() - create connection failed", __func__);
      l2cu_release_lcb(p_lcb);
      return false;
    }
  } else {
    l2cu_create_conn_br_edr(p_lcb);
  }
  return true;
}

/*******************************************************************************
 *
 *  Function        L2CA_SendFixedChnlData
 *
 *  Description     Write data on a fixed channel.
 *
 *  Parameters:     Fixed CID
 *                  BD Address of remote
 *                  Pointer to buffer of type BT_HDR
 *
 * Return value     L2CAP_DW_SUCCESS, if data accepted
 *                  L2CAP_DW_FAILED,  if error
 *
 ******************************************************************************/
uint16_t L2CA_SendFixedChnlData(uint16_t fixed_cid, const RawAddress& rem_bda,
                                BT_HDR* p_buf) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SendFixedChnlData(fixed_cid, rem_bda, p_buf);
  }

  tL2C_LCB* p_lcb;
  tBT_TRANSPORT transport = BT_TRANSPORT_BR_EDR;

  VLOG(2) << __func__ << " BDA: " << rem_bda
          << StringPrintf(" CID: 0x%04x", fixed_cid);

  if (fixed_cid >= L2CAP_ATT_CID && fixed_cid <= L2CAP_SMP_CID)
    transport = BT_TRANSPORT_LE;

  // Check CID is valid and registered
  if ((fixed_cid < L2CAP_FIRST_FIXED_CHNL) ||
      (fixed_cid > L2CAP_LAST_FIXED_CHNL) ||
      (l2cb.fixed_reg[fixed_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedData_Cb ==
       NULL)) {
    L2CAP_TRACE_ERROR("L2CA_SendFixedChnlData()  Invalid CID: 0x%04x",
                      fixed_cid);
    osi_free(p_buf);
    return (L2CAP_DW_FAILED);
  }

  // Fail if BT is not yet up
  if (!BTM_IsDeviceUp()) {
    L2CAP_TRACE_WARNING("L2CA_SendFixedChnlData(0x%04x) - BTU not ready",
                        fixed_cid);
    osi_free(p_buf);
    return (L2CAP_DW_FAILED);
  }

  // We need to have a link up
  p_lcb = l2cu_find_lcb_by_bd_addr(rem_bda, transport);
  if (p_lcb == NULL || p_lcb->link_state == LST_DISCONNECTING) {
    /* if link is disconnecting, also report data sending failure */
    L2CAP_TRACE_WARNING("L2CA_SendFixedChnlData(0x%04x) - no LCB", fixed_cid);
    osi_free(p_buf);
    return (L2CAP_DW_FAILED);
  }

  tL2C_BLE_FIXED_CHNLS_MASK peer_channel_mask;

  // Select peer channels mask to use depending on transport
  if (transport == BT_TRANSPORT_LE)
    peer_channel_mask = l2cb.l2c_ble_fixed_chnls_mask;
  else
    peer_channel_mask = p_lcb->peer_chnl_mask[0];

  if ((peer_channel_mask & (1 << fixed_cid)) == 0) {
    L2CAP_TRACE_WARNING(
        "L2CA_SendFixedChnlData() - peer does not support fixed chnl: 0x%04x",
        fixed_cid);
    osi_free(p_buf);
    return (L2CAP_DW_FAILED);
  }

  p_buf->event = 0;
  p_buf->layer_specific = L2CAP_FLUSHABLE_CH_BASED;

  if (!p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL]) {
    if (!l2cu_initialize_fixed_ccb(p_lcb, fixed_cid)) {
      L2CAP_TRACE_WARNING("L2CA_SendFixedChnlData() - no CCB for chnl: 0x%4x",
                          fixed_cid);
      osi_free(p_buf);
      return (L2CAP_DW_FAILED);
    }
  }

  // If already congested, do not accept any more packets
  if (p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL]->cong_sent) {
    L2CAP_TRACE_ERROR(
        "L2CAP - CID: 0x%04x cannot send, already congested \
            xmit_hold_q.count: %u buff_quota: %u",
        fixed_cid, fixed_queue_length(
                       p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL]
                           ->xmit_hold_q),
        p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL]->buff_quota);
    osi_free(p_buf);
    return (L2CAP_DW_FAILED);
  }

  l2c_enqueue_peer_data(p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL],
                        p_buf);

  l2c_link_check_send_pkts(p_lcb, 0, NULL);

  // If there is no dynamic CCB on the link, restart the idle timer each time
  // something is sent
  if (p_lcb->in_use && p_lcb->link_state == LST_CONNECTED &&
      !p_lcb->ccb_queue.p_first_ccb) {
    l2cu_no_dynamic_ccbs(p_lcb);
  }

  if (p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL]->cong_sent)
    return (L2CAP_DW_CONGESTED);

  return (L2CAP_DW_SUCCESS);
}

/*******************************************************************************
 *
 *  Function        L2CA_RemoveFixedChnl
 *
 *  Description     Remove a fixed channel to a remote device.
 *
 *  Parameters:     Fixed CID
 *                  BD Address of remote
 *                  Idle timeout to use (or 0xFFFF if don't care)
 *
 *  Return value:   true if channel removed
 *
 ******************************************************************************/
bool L2CA_RemoveFixedChnl(uint16_t fixed_cid, const RawAddress& rem_bda) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_RemoveFixedChnl(fixed_cid, rem_bda);
  }

  tL2C_LCB* p_lcb;
  tL2C_CCB* p_ccb;
  tBT_TRANSPORT transport = BT_TRANSPORT_BR_EDR;

  /* Check CID is valid and registered */
  if ((fixed_cid < L2CAP_FIRST_FIXED_CHNL) ||
      (fixed_cid > L2CAP_LAST_FIXED_CHNL) ||
      (l2cb.fixed_reg[fixed_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedData_Cb ==
       NULL)) {
    L2CAP_TRACE_ERROR("L2CA_RemoveFixedChnl()  Invalid CID: 0x%04x", fixed_cid);
    return (false);
  }

  if (fixed_cid >= L2CAP_ATT_CID && fixed_cid <= L2CAP_SMP_CID)
    transport = BT_TRANSPORT_LE;

  /* Is a fixed channel connected to the remote BDA ?*/
  p_lcb = l2cu_find_lcb_by_bd_addr(rem_bda, transport);

  if (((p_lcb) == NULL) ||
      (!p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL])) {
    LOG(WARNING) << __func__ << " BDA: " << rem_bda
                 << StringPrintf(" CID: 0x%04x not connected", fixed_cid);
    return (false);
  }

  VLOG(2) << __func__ << " BDA: " << rem_bda
          << StringPrintf(" CID: 0x%04x", fixed_cid);

  /* Release the CCB, starting an inactivity timeout on the LCB if no other CCBs
   * exist */
  p_ccb = p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL];

  p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL] = NULL;
  p_lcb->SetDisconnectReason(HCI_ERR_CONN_CAUSE_LOCAL_HOST);

  // Retain the link for a few more seconds after SMP pairing is done, since
  // the Android platform always does service discovery after pairing is
  // complete. This will avoid the link down (pairing is complete) and an
  // immediate re-connection for service discovery.
  // Some devices do not do auto advertising when link is dropped, thus fail
  // the second connection and service discovery.
  if ((fixed_cid == L2CAP_ATT_CID) && !p_lcb->ccb_queue.p_first_ccb)
    p_lcb->idle_timeout = 0;

  l2cu_release_ccb(p_ccb);

  return (true);
}

/*******************************************************************************
 *
 * Function         L2CA_SetFixedChannelTout
 *
 * Description      Higher layers call this function to set the idle timeout for
 *                  a fixed channel. The "idle timeout" is the amount of time
 *                  that a connection can remain up with no L2CAP channels on
 *                  it. A timeout of zero means that the connection will be torn
 *                  down immediately when the last channel is removed.
 *                  A timeout of 0xFFFF means no timeout. Values are in seconds.
 *                  A bd_addr is the remote BD address.
 *
 * Returns          true if command succeeded, false if failed
 *
 ******************************************************************************/
bool L2CA_SetFixedChannelTout(const RawAddress& rem_bda, uint16_t fixed_cid,
                              uint16_t idle_tout) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SetFixedChannelTout(rem_bda, fixed_cid,
                                                     idle_tout);
  }

  tL2C_LCB* p_lcb;
  tBT_TRANSPORT transport = BT_TRANSPORT_BR_EDR;

  if (fixed_cid >= L2CAP_ATT_CID && fixed_cid <= L2CAP_SMP_CID)
    transport = BT_TRANSPORT_LE;

  /* Is a fixed channel connected to the remote BDA ?*/
  p_lcb = l2cu_find_lcb_by_bd_addr(rem_bda, transport);
  if (((p_lcb) == NULL) ||
      (!p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL])) {
    LOG(WARNING) << __func__ << " BDA: " << rem_bda
                 << StringPrintf(" CID: 0x%04x not connected", fixed_cid);
    return (false);
  }

  p_lcb->p_fixed_ccbs[fixed_cid - L2CAP_FIRST_FIXED_CHNL]
      ->fixed_chnl_idle_tout = idle_tout;

  if (p_lcb->in_use && p_lcb->link_state == LST_CONNECTED &&
      !p_lcb->ccb_queue.p_first_ccb) {
    /* If there are no dynamic CCBs, (re)start the idle timer in case we changed
     * it */
    l2cu_no_dynamic_ccbs(p_lcb);
  }

  return true;
}

/*******************************************************************************
 *
 * Function         L2CA_DataWrite
 *
 * Description      Higher layers call this function to write data.
 *
 * Returns          L2CAP_DW_SUCCESS, if data accepted, else false
 *                  L2CAP_DW_CONGESTED, if data accepted and the channel is
 *                                      congested
 *                  L2CAP_DW_FAILED, if error
 *
 ******************************************************************************/
uint8_t L2CA_DataWrite(uint16_t cid, BT_HDR* p_data) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_DataWrite(cid, p_data);
  }

  L2CAP_TRACE_API("L2CA_DataWrite()  CID: 0x%04x  Len: %d", cid, p_data->len);
  return l2c_data_write(cid, p_data, L2CAP_FLUSHABLE_CH_BASED);
}

/*******************************************************************************
 *
 * Function         L2CA_SetChnlFlushability
 *
 * Description      Higher layers call this function to set a channels
 *                  flushability flags
 *
 * Returns          true if CID found, else false
 *
 ******************************************************************************/
bool L2CA_SetChnlFlushability(uint16_t cid, bool is_flushable) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_SetChnlFlushability(cid, is_flushable);
  }

  tL2C_CCB* p_ccb;

  /* Find the channel control block. We don't know the link it is on. */
  p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
  if (p_ccb == NULL) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_SetChnlFlushability, CID: %d",
                        cid);
    return (false);
  }

  p_ccb->is_flushable = is_flushable;

  L2CAP_TRACE_API("L2CA_SetChnlFlushability()  CID: 0x%04x  is_flushable: %d",
                  cid, is_flushable);

  return (true);
}

/*******************************************************************************
 *
 * Function     L2CA_FlushChannel
 *
 * Description  This function flushes none, some or all buffers queued up
 *              for xmission for a particular CID. If called with
 *              L2CAP_FLUSH_CHANS_GET (0), it simply returns the number
 *              of buffers queued for that CID L2CAP_FLUSH_CHANS_ALL (0xffff)
 *              flushes all buffers.  All other values specifies the maximum
 *              buffers to flush.
 *
 * Returns      Number of buffers left queued for that CID
 *
 ******************************************************************************/
uint16_t L2CA_FlushChannel(uint16_t lcid, uint16_t num_to_flush) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_FlushChannel(lcid, num_to_flush);
  }

  tL2C_CCB* p_ccb;
  tL2C_LCB* p_lcb;
  uint16_t num_left = 0, num_flushed1 = 0, num_flushed2 = 0;

  p_ccb = l2cu_find_ccb_by_cid(NULL, lcid);

  if (!p_ccb || (p_ccb->p_lcb == NULL)) {
    L2CAP_TRACE_WARNING(
        "L2CA_FlushChannel()  abnormally returning 0  CID: 0x%04x", lcid);
    return (0);
  }
  p_lcb = p_ccb->p_lcb;

  if (num_to_flush != L2CAP_FLUSH_CHANS_GET) {
    L2CAP_TRACE_API(
        "L2CA_FlushChannel (FLUSH)  CID: 0x%04x  NumToFlush: %d  QC: %u  "
        "pFirst: 0x%08x",
        lcid, num_to_flush, fixed_queue_length(p_ccb->xmit_hold_q),
        fixed_queue_try_peek_first(p_ccb->xmit_hold_q));
  } else {
    L2CAP_TRACE_API("L2CA_FlushChannel (QUERY)  CID: 0x%04x", lcid);
  }

  /* Cannot flush eRTM buffers once they have a sequence number */
  if (p_ccb->peer_cfg.fcr.mode != L2CAP_FCR_ERTM_MODE) {
    const controller_t* controller = controller_get_interface();
    if (num_to_flush != L2CAP_FLUSH_CHANS_GET) {
      /* If the controller supports enhanced flush, flush the data queued at the
       * controller */
      if (controller->supports_non_flushable_pb() &&
          (BTM_GetNumScoLinks() == 0)) {
        /* The only packet type defined - 0 - Automatically-Flushable Only */
        btsnd_hcic_enhanced_flush(p_lcb->Handle(), 0);
      }
    }

    // Iterate though list and flush the amount requested from
    // the transmit data queue that satisfy the layer and event conditions.
    for (const list_node_t* node = list_begin(p_lcb->link_xmit_data_q);
         (num_to_flush > 0) && node != list_end(p_lcb->link_xmit_data_q);) {
      BT_HDR* p_buf = (BT_HDR*)list_node(node);
      node = list_next(node);
      if ((p_buf->layer_specific == 0) && (p_buf->event == lcid)) {
        num_to_flush--;
        num_flushed1++;

        list_remove(p_lcb->link_xmit_data_q, p_buf);
        osi_free(p_buf);
      }
    }
  }

  /* If needed, flush buffers in the CCB xmit hold queue */
  while ((num_to_flush != 0) && (!fixed_queue_is_empty(p_ccb->xmit_hold_q))) {
    BT_HDR* p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_ccb->xmit_hold_q);
    osi_free(p_buf);
    num_to_flush--;
    num_flushed2++;
  }

  /* If app needs to track all packets, call it */
  if ((p_ccb->p_rcb) && (p_ccb->p_rcb->api.pL2CA_TxComplete_Cb) &&
      (num_flushed2))
    (*p_ccb->p_rcb->api.pL2CA_TxComplete_Cb)(p_ccb->local_cid, num_flushed2);

  /* Now count how many are left */
  for (const list_node_t* node = list_begin(p_lcb->link_xmit_data_q);
       node != list_end(p_lcb->link_xmit_data_q); node = list_next(node)) {
    BT_HDR* p_buf = (BT_HDR*)list_node(node);
    if (p_buf->event == lcid) num_left++;
  }

  /* Add in the number in the CCB xmit queue */
  num_left += fixed_queue_length(p_ccb->xmit_hold_q);

  /* Return the local number of buffers left for the CID */
  L2CAP_TRACE_DEBUG("L2CA_FlushChannel()  flushed: %u + %u,  num_left: %u",
                    num_flushed1, num_flushed2, num_left);

  /* If we were congested, and now we are not, tell the app */
  l2cu_check_channel_congestion(p_ccb);

  return (num_left);
}

bool L2CA_IsLinkEstablished(const RawAddress& bd_addr,
                            tBT_TRANSPORT transport) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::L2CA_IsLinkEstablished(bd_addr, transport);
  }

  return l2cu_find_lcb_by_bd_addr(bd_addr, transport) != nullptr;
}
