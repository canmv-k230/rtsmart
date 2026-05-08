// SPDX-License-Identifier: BSD-3-Clause
/*
 * core_intr.c - DesignWare HS OTG Controller common interrupt handling
 *
 * Copyright (C) 2004-2013 Synopsys, Inc.
 */

/*
 * This file contains the common interrupt handlers
 *
 * This file is licensed under the BSD-3-Clause License.
 * You may obtain a copy of the License at:
 * https://opensource.org/licenses/BSD-3-Clause
 *
 */

#include "core.h"
#include "hcd.h"

static const char *dwc2_op_state_str(struct dwc2_hsotg *hsotg)
{
    switch (hsotg->op_state) {
    case OTG_STATE_A_HOST:
        return "a_host";
    case OTG_STATE_A_SUSPEND:
        return "a_suspend";
    case OTG_STATE_A_PERIPHERAL:
        return "a_peripheral";
    case OTG_STATE_B_PERIPHERAL:
        return "b_peripheral";
    case OTG_STATE_B_HOST:
        return "b_host";
    default:
        return "unknown";
    }
}

#define GINTMSK_COMMON	(GINTSTS_WKUPINT | GINTSTS_SESSREQINT |		\
                         GINTSTS_CONIDSTSCHNG | GINTSTS_OTGINT |	\
                         GINTSTS_MODEMIS | GINTSTS_DISCONNINT |		\
                         GINTSTS_USBSUSP | GINTSTS_PRTINT |		\
                         GINTSTS_LPMTRANRCVD)

/*
 * This function returns the Core Interrupt register
 */
static u32 dwc2_read_common_intr(struct dwc2_hsotg *hsotg)
{
    u32 gintsts;
    u32 gintmsk;
    u32 gahbcfg;
    u32 gintmsk_common = GINTMSK_COMMON;

    gintsts = dwc2_readl(hsotg, GINTSTS);
    gintmsk = dwc2_readl(hsotg, GINTMSK);
    gahbcfg = dwc2_readl(hsotg, GAHBCFG);

    /* If any common interrupts set */
    if (gintsts & gintmsk_common)
        dev_dbg(hsotg->dev, "gintsts=%08x  gintmsk=%08x\n",
                gintsts, gintmsk);

    if (gahbcfg & GAHBCFG_GLBL_INTR_EN)
        return gintsts & gintmsk & gintmsk_common;
    else
        return 0;
}

/**
 * dwc_handle_gpwrdn_disc_det() - Handles the gpwrdn disconnect detect.
 * Exits hibernation without restoring registers.
 *
 * @hsotg: Programming view of DWC_otg controller
 * @gpwrdn: GPWRDN register
 */
static inline void dwc_handle_gpwrdn_disc_det(struct dwc2_hsotg *hsotg,
                                              u32 gpwrdn)
{
    u32 gpwrdn_tmp;

    /* Switch-on voltage to the core */
    gpwrdn_tmp = dwc2_readl(hsotg, GPWRDN);
    gpwrdn_tmp &= ~GPWRDN_PWRDNSWTCH;
    dwc2_writel(hsotg, gpwrdn_tmp, GPWRDN);
    udelay(5);

    /* Reset core */
    gpwrdn_tmp = dwc2_readl(hsotg, GPWRDN);
    gpwrdn_tmp &= ~GPWRDN_PWRDNRSTN;
    dwc2_writel(hsotg, gpwrdn_tmp, GPWRDN);
    udelay(5);

    /* Disable Power Down Clamp */
    gpwrdn_tmp = dwc2_readl(hsotg, GPWRDN);
    gpwrdn_tmp &= ~GPWRDN_PWRDNCLMP;
    dwc2_writel(hsotg, gpwrdn_tmp, GPWRDN);
    udelay(5);

    /* Deassert reset core */
    gpwrdn_tmp = dwc2_readl(hsotg, GPWRDN);
    gpwrdn_tmp |= GPWRDN_PWRDNRSTN;
    dwc2_writel(hsotg, gpwrdn_tmp, GPWRDN);
    udelay(5);

    /* Disable PMU interrupt */
    gpwrdn_tmp = dwc2_readl(hsotg, GPWRDN);
    gpwrdn_tmp &= ~GPWRDN_PMUINTSEL;
    dwc2_writel(hsotg, gpwrdn_tmp, GPWRDN);

    /* Reset ULPI latch */
    gpwrdn = dwc2_readl(hsotg, GPWRDN);
    gpwrdn &= ~GPWRDN_ULPI_LATCH_EN_DURING_HIB_ENTRY;
    dwc2_writel(hsotg, gpwrdn, GPWRDN);

    /* De-assert Wakeup Logic */
    gpwrdn_tmp = dwc2_readl(hsotg, GPWRDN);
    gpwrdn_tmp &= ~GPWRDN_PMUACTV;
    dwc2_writel(hsotg, gpwrdn_tmp, GPWRDN);

    hsotg->hibernated = 0;
    hsotg->bus_suspended = 0;

    if (gpwrdn & GPWRDN_IDSTS) {
        dev_err(hsotg->dev, "in devce mode %s\n", __func__);
    } else {
        hsotg->op_state = OTG_STATE_A_HOST;

        /* Initialize the Core for Host mode */
        dwc2_core_init(hsotg, false);
        dwc2_enable_global_interrupts(hsotg);
        dwc2_hcd_start(hsotg);
    }
}

/*
 * GPWRDN interrupt handler.
 *
 * The GPWRDN interrupts are those that occur in both Host and
 * Device mode while core is in hibernated state.
 */
static int dwc2_handle_gpwrdn_intr(struct dwc2_hsotg *hsotg)
{
    u32 gpwrdn;
    int linestate;
    int ret = 0;

    gpwrdn = dwc2_readl(hsotg, GPWRDN);
    /* clear all interrupt */
    dwc2_writel(hsotg, gpwrdn, GPWRDN);
    linestate = (gpwrdn & GPWRDN_LINESTATE_MASK) >> GPWRDN_LINESTATE_SHIFT;
    dev_dbg(hsotg->dev,
            "%s: dwc2_handle_gpwrdwn_intr called gpwrdn= %08x\n", __func__,
            gpwrdn);

    if ((gpwrdn & GPWRDN_DISCONN_DET) &&
        (gpwrdn & GPWRDN_DISCONN_DET_MSK) && !linestate) {
        dev_dbg(hsotg->dev, "%s: GPWRDN_DISCONN_DET\n", __func__);
        /*
         * Call disconnect detect function to exit from
         * hibernation
         */
        dwc_handle_gpwrdn_disc_det(hsotg, gpwrdn);
    } else if ((gpwrdn & GPWRDN_LNSTSCHG) &&
               (gpwrdn & GPWRDN_LNSTSCHG_MSK) && linestate) {
        dev_dbg(hsotg->dev, "%s: GPWRDN_LNSTSCHG\n", __func__);
        if (hsotg->hw_params.hibernation &&
            hsotg->hibernated) {
            if (gpwrdn & GPWRDN_IDSTS) {
                ret = dwc2_exit_hibernation(hsotg, 0, 0, 0);
                if (ret)
                    dev_err(hsotg->dev,
                            "exit hibernation failed.\n");
                call_gadget(hsotg, resume);
            } else {
                ret = dwc2_exit_hibernation(hsotg, 1, 0, 1);
                if (ret)
                    dev_err(hsotg->dev,
                            "exit hibernation failed.\n");
            }
        }
    } else if ((gpwrdn & GPWRDN_RST_DET) &&
               (gpwrdn & GPWRDN_RST_DET_MSK)) {
        dev_dbg(hsotg->dev, "%s: GPWRDN_RST_DET\n", __func__);
        if (!linestate) {
            ret = dwc2_exit_hibernation(hsotg, 0, 1, 0);
            if (ret)
                dev_err(hsotg->dev,
                        "exit hibernation failed.\n");
        }
    } else if ((gpwrdn & GPWRDN_STS_CHGINT) &&
               (gpwrdn & GPWRDN_STS_CHGINT_MSK)) {
        dev_dbg(hsotg->dev, "%s: GPWRDN_STS_CHGINT\n", __func__);
        /*
         * As GPWRDN_STS_CHGINT exit from hibernation flow is
         * the same as in GPWRDN_DISCONN_DET flow. Call
         * disconnect detect helper function to exit from
         * hibernation.
         */
        dwc_handle_gpwrdn_disc_det(hsotg, gpwrdn);
    }

    return ret;
}

/**
 * dwc2_handle_mode_mismatch_intr() - Logs a mode mismatch warning message
 *
 * @hsotg: Programming view of DWC_otg controller
 */
static void dwc2_handle_mode_mismatch_intr(struct dwc2_hsotg *hsotg)
{
    /* Clear interrupt */
    dwc2_writel(hsotg, GINTSTS_MODEMIS, GINTSTS);

    dev_warn(hsotg->dev, "Mode Mismatch Interrupt: currently in %s mode\n",
             dwc2_is_host_mode(hsotg) ? "Host" : "Device");
}

/**
 * dwc2_handle_otg_intr() - Handles the OTG Interrupts. It reads the OTG
 * Interrupt Register (GOTGINT) to determine what interrupt has occurred.
 *
 * @hsotg: Programming view of DWC_otg controller
 */
static void dwc2_handle_otg_intr(struct dwc2_hsotg *hsotg)
{
    dev_err(hsotg->dev, "++OTG Interrupt ..............\n");
}

/**
 * dwc2_handle_conn_id_status_change_intr() - Handles the Connector ID Status
 * Change Interrupt
 *
 * @hsotg: Programming view of DWC_otg controller
 *
 * Reads the OTG Interrupt Register (GOTCTL) to determine whether this is a
 * Device to Host Mode transition or a Host to Device Mode transition. This only
 * occurs when the cable is connected/removed from the PHY connector.
 */
static void dwc2_handle_conn_id_status_change_intr(struct dwc2_hsotg *hsotg)
{
    u32 gintmsk;

    /* Clear interrupt */
    dwc2_writel(hsotg, GINTSTS_CONIDSTSCHNG, GINTSTS);

    /* Need to disable SOF interrupt immediately */
    gintmsk = dwc2_readl(hsotg, GINTMSK);
    gintmsk &= ~GINTSTS_SOF;
    dwc2_writel(hsotg, gintmsk, GINTMSK);

    dev_dbg(hsotg->dev, " ++Connector ID Status Change Interrupt++  (%s)\n",
            dwc2_is_host_mode(hsotg) ? "Host" : "Device");

    /*
     * Need to schedule a work, as there are possible DELAY function calls.
     */
    if (hsotg->wq_otg)
        rt_workqueue_submit_work(hsotg->wq_otg, &hsotg->wf_otg, 0);
}

/*
 * This interrupt indicates that a device has been disconnected from the
 * root port
 */
static void dwc2_handle_disconnect_intr(struct dwc2_hsotg *hsotg)
{
    dwc2_writel(hsotg, GINTSTS_DISCONNINT, GINTSTS);

#if 0
    if (hsotg->op_state == OTG_STATE_A_HOST)
#endif
        dwc2_hcd_disconnect(hsotg, false);

    dev_err(hsotg->dev, "Disconn (%s) %s\n",
            dwc2_is_host_mode(hsotg) ? "Host" : "Device",
            dwc2_op_state_str(hsotg));
}


/**
 * dwc2_handle_session_req_intr() - This interrupt indicates that a device is
 * initiating the Session Request Protocol to request the host to turn on bus
 * power so a new session can begin
 *
 * @hsotg: Programming view of DWC_otg controller
 *
 * This handler responds by turning on bus power. If the DWC_otg controller is
 * in low power mode, this handler brings the controller out of low power mode
 * before turning on bus power.
 */
static void dwc2_handle_session_req_intr(struct dwc2_hsotg *hsotg)
{
    dev_err(hsotg->dev, "Session request interrupt\n");
    /* Clear interrupt */
    dwc2_writel(hsotg, GINTSTS_SESSREQINT, GINTSTS);
}

static void dwc2_handle_wakeup_detected_intr(struct dwc2_hsotg *hsotg)
{
    dev_err(hsotg->dev, "++Resume or Remote Wakeup Detected Interrupt++\n");
    /* Clear interrupt */
    dwc2_writel(hsotg, GINTSTS_WKUPINT, GINTSTS);
}

static void dwc2_handle_usb_suspend_intr(struct dwc2_hsotg *hsotg)
{
    dev_err(hsotg->dev, "USB SUSPEND\n");
    /* Clear interrupt */
    dwc2_writel(hsotg, GINTSTS_USBSUSP, GINTSTS);
}

static void dwc2_handle_lpm_intr(struct dwc2_hsotg *hsotg)
{
    dev_err(hsotg->dev, "USB LPM\n");
    /* Clear interrupt */
    dwc2_writel(hsotg, GINTSTS_LPMTRANRCVD, GINTSTS);
}

/*
 * Common interrupt handler
 *
 * The common interrupts are those that occur in both Host and Device mode.
 * This handler handles the following interrupts:
 * - Mode Mismatch Interrupt
 * - OTG Interrupt
 * - Connector ID Status Change Interrupt
 * - Disconnect Interrupt
 * - Session Request Interrupt
 * - Resume / Remote Wakeup Detected Interrupt
 * - Suspend Interrupt
 */
irqreturn_t dwc2_handle_common_intr(struct dwc2_hsotg *hsotg)
{
    u32 gintsts;
    irqreturn_t retval = IRQ_NONE;

    rt_spin_lock(&hsotg->lock);

    if (!dwc2_is_controller_alive(hsotg)) {
        dev_warn(hsotg->dev, "Controller is dead\n");
        goto out;
    }

    /* Reading current frame number value host modes. */
    if (dwc2_is_device_mode(hsotg))
        dev_err(hsotg->dev, "in device mode\n");
    else
        hsotg->frame_number = (dwc2_readl(hsotg, HFNUM)
                               & HFNUM_FRNUM_MASK) >> HFNUM_FRNUM_SHIFT;

    gintsts = dwc2_read_common_intr(hsotg);
    if (gintsts & ~GINTSTS_PRTINT)
        retval = IRQ_HANDLED;

    /* In case of hibernated state gintsts must not work */
    if (hsotg->hibernated) {
        dwc2_handle_gpwrdn_intr(hsotg);
        retval = IRQ_HANDLED;
        goto out;
    }

    if (gintsts & GINTSTS_MODEMIS)
        dwc2_handle_mode_mismatch_intr(hsotg);
    if (gintsts & GINTSTS_OTGINT)
        dwc2_handle_otg_intr(hsotg);
    if (gintsts & GINTSTS_CONIDSTSCHNG)
        dwc2_handle_conn_id_status_change_intr(hsotg);
    if (gintsts & GINTSTS_DISCONNINT)
        dwc2_handle_disconnect_intr(hsotg);
    if (gintsts & GINTSTS_SESSREQINT)
        dwc2_handle_session_req_intr(hsotg);
    if (gintsts & GINTSTS_WKUPINT)
        dwc2_handle_wakeup_detected_intr(hsotg);
    if (gintsts & GINTSTS_USBSUSP)
        dwc2_handle_usb_suspend_intr(hsotg);
    if (gintsts & GINTSTS_LPMTRANRCVD)
        dwc2_handle_lpm_intr(hsotg);

    if (gintsts & GINTSTS_PRTINT) {
        /*
         * The port interrupt occurs while in device mode with HPRT0
         * Port Enable/Disable
         */
        if (dwc2_is_device_mode(hsotg)) {
            dev_err(hsotg->dev,
                    " --Port interrupt received in Device mode--\n");
        }
    }

out:
    rt_spin_unlock(&hsotg->lock);
    return retval;
}
