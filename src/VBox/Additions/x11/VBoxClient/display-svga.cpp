/* $Id: display-svga.cpp $ */
/** @file
 * X11 guest client - VMSVGA emulation resize event pass-through to drm guest
 * driver.
 */

/*
 * Copyright (C) 2016-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*
 * Known things to test when changing this code.  All assume a guest with VMSVGA
 * active and controlled by X11 or Wayland, and Guest Additions installed and
 * running, unless otherwise stated.
 *  - On Linux 4.6 and later guests, VBoxClient --vmsvga should be running as
 *    root and not as the logged-in user.  Dynamic resizing should work for all
 *    screens in any environment which handles kernel resize notifications,
 *    including at log-in screens.  Test GNOME Shell Wayland and GNOME Shell
 *    under X.Org or Unity or KDE at the log-in screen and after log-in.
 *  - Linux 4.10 changed the user-kernel-ABI introduced in 4.6: test both.
 *  - On other guests (than Linux 4.6 or later) running X.Org Server 1.3 or
 *    later, VBoxClient --vmsvga should never be running as root, and should run
 *    (and dynamic resizing and screen enable/disable should work for all
 *    screens) whenever a user is logged in to a supported desktop environment.
 *  - On guests running X.Org Server 1.2 or older, VBoxClient --vmsvga should
 *    never run as root and should run whenever a user is logged in to a
 *    supported desktop environment.  Dynamic resizing should work for the first
 *    screen, and enabling others should not be possible.
 *  - When VMSVGA is not enabled, VBoxClient --vmsvga should never stay running.
 */

#include "VBoxClient.h"

#include <VBox/VBoxGuestLib.h>

#include <iprt/assert.h>
#include <iprt/file.h>
#include <iprt/err.h>
#include <iprt/string.h>

#include <stdio.h>

/** Maximum number of supported screens.  DRM and X11 both limit this to 32. */
/** @todo if this ever changes, dynamically allocate resizeable arrays in the
 *  context structure. */
#define VMW_MAX_HEADS 32

/* VMWare kernel driver control parts definitions. */

#ifdef RT_OS_LINUX
# include <sys/ioctl.h>
#else  /* Solaris and BSDs, in case they ever adopt the DRM driver. */
# include <sys/ioccom.h>
#endif

#define DRM_DRIVER_NAME "vmwgfx"

/** DRM version structure. */
struct DRMVERSION
{
    int cMajor;
    int cMinor;
    int cPatchLevel;
    size_t cbName;
    char *pszName;
    size_t cbDate;
    char *pszDate;
    size_t cbDescription;
    char *pszDescription;
};
AssertCompileSize(struct DRMVERSION, 8 + 7 * sizeof(void *));

/** Rectangle structure for geometry of a single screen. */
struct DRMVMWRECT
{
        int32_t x;
        int32_t y;
        uint32_t w;
        uint32_t h;
};
AssertCompileSize(struct DRMVMWRECT, 16);

#define DRM_IOCTL_VERSION _IOWR('d', 0x00, struct DRMVERSION)

struct DRMCONTEXT
{
    RTFILE hDevice;
};

static void drmConnect(struct DRMCONTEXT *pContext)
{
    unsigned i;
    RTFILE hDevice;

    if (pContext->hDevice != NIL_RTFILE)
        VBClFatalError(("%s called with bad argument\n", __func__));
    /* Try to open the SVGA DRM device. */
    for (i = 0; i < 128; ++i)
    {
        char szPath[64];
        struct DRMVERSION version;
        char szName[sizeof(DRM_DRIVER_NAME)];
        int rc;

        /* Control devices for drm graphics driver control devices go from
         * controlD64 to controlD127.  Render node devices go from renderD128
         * to renderD192.  The driver takes resize hints via the control device
         * on pre-4.10 kernels and on the render device on newer ones.  Try
         * both types. */
        if (i % 2 == 0)
            rc = RTStrPrintf(szPath, sizeof(szPath), "/dev/dri/renderD%u", i / 2 + 128);
        else
            rc = RTStrPrintf(szPath, sizeof(szPath), "/dev/dri/controlD%u", i / 2 + 64);
        if (RT_FAILURE(rc))
            VBClFatalError(("RTStrPrintf of device path failed, rc=%Rrc\n", rc));
        rc = RTFileOpen(&hDevice, szPath, RTFILE_O_READWRITE | RTFILE_O_OPEN | RTFILE_O_DENY_NONE);
        if (RT_FAILURE(rc))
            continue;
        RT_ZERO(version);
        version.cbName = sizeof(szName);
        version.pszName = szName;
        rc = RTFileIoCtl(hDevice, DRM_IOCTL_VERSION, &version, sizeof(version), NULL);
        if (   RT_SUCCESS(rc)
            && !strncmp(szName, DRM_DRIVER_NAME, sizeof(DRM_DRIVER_NAME) - 1)
            && (   version.cMajor > 2
                || (version.cMajor == 2 && version.cMinor > 9)))
            break;
        hDevice = NIL_RTFILE;
    }
    pContext->hDevice = hDevice;
}

/** Preferred screen layout information for DRM_VMW_UPDATE_LAYOUT IoCtl.  The
 *  rects argument is a cast pointer to an array of drm_vmw_rect. */
struct DRMVMWUPDATELAYOUT {
        uint32_t cOutputs;
        uint32_t u32Pad;
        uint64_t ptrRects;
};
AssertCompileSize(struct DRMVMWUPDATELAYOUT, 16);

#define DRM_IOCTL_VMW_UPDATE_LAYOUT \
        _IOW('d', 0x40 + 20, struct DRMVMWUPDATELAYOUT)

static void drmSendHints(struct DRMCONTEXT *pContext, struct DRMVMWRECT *paRects,
                         unsigned cHeads)
{
    int rc;
    struct DRMVMWUPDATELAYOUT ioctlLayout;

    if (pContext->hDevice == NIL_RTFILE)
        VBClFatalError(("%s bad device argument.\n", __func__));
    ioctlLayout.cOutputs = cHeads;
    ioctlLayout.ptrRects = (uint64_t)paRects;
    rc = RTFileIoCtl(pContext->hDevice, DRM_IOCTL_VMW_UPDATE_LAYOUT,
                     &ioctlLayout, sizeof(ioctlLayout), NULL);
    if (RT_FAILURE(rc) && rc != VERR_INVALID_PARAMETER)
        VBClFatalError(("Failure updating layout, rc=%Rrc\n", rc));
}

static const char *getPidFilePath()
{
    return ".vboxclient-display-svga.pid";
}

static int run(struct VBCLSERVICE **ppInterface, bool fDaemonised)
{
    (void)ppInterface;
    (void)fDaemonised;
    struct DRMCONTEXT drmContext = { NIL_RTFILE };
    unsigned i;
    int rc;
    struct DRMVMWRECT aRects[VMW_MAX_HEADS];
    unsigned cHeads;
    /* Do not acknowledge the first event we query for to pick up old events,
     * e.g. from before a guest reboot. */
    bool fAck = false;

    drmConnect(&drmContext);
    if (drmContext.hDevice == NIL_RTFILE)
        return VINF_SUCCESS;
    /* Initialise the guest library. */
    rc = VbglR3InitUser();
    if (RT_FAILURE(rc))
        VBClFatalError(("Failed to connect to the VirtualBox kernel service, rc=%Rrc\n", rc));
    rc = VbglR3CtlFilterMask(VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, 0);
    if (RT_FAILURE(rc))
        VBClFatalError(("Failed to request display change events, rc=%Rrc\n", rc));
    rc = VbglR3AcquireGuestCaps(VMMDEV_GUEST_SUPPORTS_GRAPHICS, 0, false);
    if (rc == VERR_RESOURCE_BUSY)  /* Someone else has already acquired it. */
        return VINF_SUCCESS;
    if (RT_FAILURE(rc))
        VBClFatalError(("Failed to register resizing support, rc=%Rrc\n", rc));
    for (;;)
    {
        uint32_t events;
        struct VMMDevDisplayDef aDisplays[VMW_MAX_HEADS];
        uint32_t cDisplaysOut;

        /* Query the first size without waiting.  This lets us e.g. pick up
         * the last event before a guest reboot when we start again after. */
        rc = VbglR3GetDisplayChangeRequestMulti(VMW_MAX_HEADS, &cDisplaysOut, aDisplays, fAck);
        fAck = true;
        if (RT_FAILURE(rc))
            VBClFatalError(("Failed to get display change request, rc=%Rrc\n", rc));
        if (cDisplaysOut > VMW_MAX_HEADS)
            VBClFatalError(("Display change request contained, rc=%Rrc\n", rc));
        for (i = 0, cHeads = 0; i < cDisplaysOut && i < VMW_MAX_HEADS; ++i)
        {
            if (!(aDisplays[i].fDisplayFlags & VMMDEV_DISPLAY_DISABLED))
            {
                if ((i == 0) || (aDisplays[i].fDisplayFlags & VMMDEV_DISPLAY_ORIGIN))
                {
                    aRects[cHeads].x = aDisplays[i].xOrigin;
                    aRects[cHeads].y = aDisplays[i].yOrigin;
                } else {
                    aRects[cHeads].x = aRects[cHeads - 1].x + aRects[cHeads - 1].w;
                    aRects[cHeads].y = aRects[cHeads - 1].y;
                }
                aRects[cHeads].w = aDisplays[i].cx;
                aRects[cHeads].h = aDisplays[i].cy;
                ++cHeads;
            }
        }
        for (i = 0; i < cHeads; ++i)
            printf("Head %u: %dx%d, (%d, %d)\n", i, (int)aRects[i].w, (int)aRects[i].h,
                   (int)aRects[i].x, (int)aRects[i].y);
        drmSendHints(&drmContext, aRects, cHeads);
        rc = VbglR3WaitEvent(VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, RT_INDEFINITE_WAIT, &events);
        if (RT_FAILURE(rc))
            VBClFatalError(("Failure waiting for event, rc=%Rrc\n", rc));
    }
}

static struct VBCLSERVICE interface =
{
    getPidFilePath,
    VBClServiceDefaultHandler, /* Init */
    run,
    VBClServiceDefaultCleanup
}, *pInterface = &interface;

struct VBCLSERVICE **VBClDisplaySVGAService()
{
    return &pInterface;
}
