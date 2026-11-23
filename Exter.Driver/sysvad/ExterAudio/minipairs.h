/*++

Copyright (c) Microsoft Corporation All Rights Reserved
Modified for the Exter Virtual Audio Device (Exter Interactive)

Module Name:

    minipairs.h

Abstract:

    EXTER VIRTUAL MICROPHONE: hanya SATU endpoint (capture, mic-in) yang didaftarkan.
    Semua endpoint render (speaker/headphone/HDMI/SPDIF), mic array, Bluetooth HFP, USB headset, dan A2DP
    bawaan SysVAD dihapus. Suara yang keluar dari endpoint ini berasal dari ring shared-memory Exter
    (lihat EndpointsCommon/ExterVmic.cpp).

--*/

#ifndef _SYSVAD_MINIPAIRS_H_
#define _SYSVAD_MINIPAIRS_H_

#include "micintopo.h"
#include "micintoptable.h"
#include "micinwavtable.h"

NTSTATUS
CreateMiniportWaveRTSYSVAD
( 
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

NTSTATUS
CreateMiniportTopologySYSVAD
( 
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

//
// Capture miniports.
//

/*********************************************************************
* Topology/Wave bridge connection for mic in                         *
*                                                                    *
*              +------+    +------+                                  *
*              | Topo |    | Wave |                                  *
*              |      |    |      |                                  *
*  Mic in  --->|0    1|===>|0    1|---> Capture Host Pin             *
*              |      |    |      |                                  *
*              +------+    +------+                                  *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE MicInTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_BRIDGE,          // TopologyOut
        KSPIN_WAVE_BRIDGE,          // WaveIn
        CONNECTIONTYPE_TOPOLOGY_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR MicInMiniports =
{
    eMicInDevice,
    L"TopologyMicIn",                       // make sure this or the template name matches with KSNAME_TopologyMicIn in the inf's [Strings] section 
    NULL,                                   // optional template name
    CreateMiniportTopologySYSVAD,
    &MicInTopoMiniportFilterDescriptor,
    0, NULL,                                // Interface properties
    L"WaveMicIn",                           // make sure this or the template name matches with KSNAME_WaveMicIn in the inf's [Strings] section
    NULL,                                   // optional template name
    CreateMiniportWaveRTSYSVAD,
    &MicInWaveMiniportFilterDescriptor,
    0, NULL,                                // Interface properties
    MICIN_DEVICE_MAX_CHANNELS,
    MicInPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicInPinDeviceFormatsAndModes),
    MicInTopologyPhysicalConnections,
    SIZEOF_ARRAY(MicInTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
    NULL, 0, NULL,                          // audio module settings.
};

//=============================================================================
//
// Capture miniport pairs. (Tidak ada render endpoint.)
//
static
PENDPOINT_MINIPAIR  g_CaptureEndpoints[] = 
{
    &MicInMiniports,
};

#define g_cCaptureEndpoints (SIZEOF_ARRAY(g_CaptureEndpoints))

//=============================================================================
//
// Total miniports = # endpoints * 2 (topology + wave).
//
#define g_MaxMiniports  (g_cCaptureEndpoints * 2)

#endif // _SYSVAD_MINIPAIRS_H_
