/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stddef.h>
#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "devpropdef.h"
#include "devfiltertypes.h"
#include "devquery.h"
#include "hidusage.h"
#include "xinput.h"
#include "ddk/hidsdi.h"

#include "initguid.h"
#include "gameinput.h"
#include "devpkey.h"
#include "ddk/hidclass.h"

#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ginput);

DEFINE_GUID( GUID_DEVINTERFACE_WINEXINPUT,0x6c53d5fd,0x6480,0x440f,0xb6,0x18,0x47,0x67,0x50,0xc5,0xe1,0xa6 );

static CRITICAL_SECTION game_input_cs;
static CRITICAL_SECTION_DEBUG game_input_cs_debug =
{
    0, 0, &game_input_cs,
    { &game_input_cs_debug.ProcessLocksList, &game_input_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": game_input_cs") }
};
static CRITICAL_SECTION game_input_cs = { &game_input_cs_debug, -1, 0, 0, 0, 0 };
static struct game_input *game_input;

/* Held while device callbacks are delivered -- by the pad notification thread,
 * and by a registration reporting what is already connected -- so that
 * unregistering one can wait for a delivery in progress to finish, and the two
 * deliveries to one callback cannot overtake each other. */
static CRITICAL_SECTION device_callback_cs;
static CRITICAL_SECTION_DEBUG device_callback_cs_debug =
{
    0, 0, &device_callback_cs,
    { &device_callback_cs_debug.ProcessLocksList, &device_callback_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": device_callback_cs") }
};
static CRITICAL_SECTION device_callback_cs = { &device_callback_cs_debug, -1, 0, 0, 0, 0 };

struct device
{
    IGameInputDevice_v0 IGameInputDevice_v0_iface;
    LONG refcount;
    WCHAR path[MAX_PATH];
    struct list entry;

    GameInputDeviceStatus status;
    GameInputDeviceInfo_v0 info_v0;
};

static struct device *device_from_IGameInputDevice_v0( IGameInputDevice_v0 *iface )
{
    return CONTAINING_RECORD( iface, struct device, IGameInputDevice_v0_iface );
}

static HRESULT WINAPI game_input_device_v0_QueryInterface( IGameInputDevice_v0 *iface, REFIID iid, void **out )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );

    TRACE( "device %p, iid %s, out %p.\n", device, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IGameInputDevice_v0 ) ||
        IsEqualGUID( iid, &IID_IUnknown ))
    {
        IGameInputDevice_v0_AddRef( &device->IGameInputDevice_v0_iface );
        *out = &device->IGameInputDevice_v0_iface;
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI game_input_device_v0_AddRef( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    ULONG ref = InterlockedIncrement( &device->refcount );
    TRACE( "device %p increasing refcount to %lu.\n", device, ref );
    return ref;
}

static ULONG WINAPI game_input_device_v0_Release( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    ULONG ref = InterlockedDecrement( &device->refcount );
    TRACE( "device %p decreasing refcount to %lu.\n", device, ref );
    if (!ref) free( device );
    return ref;
};

static const GameInputDeviceInfo_v0 *WINAPI game_input_device_v0_GetDeviceInfo( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    FIXME( "device %p stub!\n", device );
    return &device->info_v0;
}

static GameInputDeviceStatus WINAPI game_input_device_v0_GetDeviceStatus( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
    return 0;
}

static void WINAPI game_input_device_v0_GetBatteryState( IGameInputDevice_v0 *iface, GameInputBatteryState *state )
{
    FIXME( "device %p, state %p stub!\n", device_from_IGameInputDevice_v0( iface ), state );
}

static HRESULT WINAPI game_input_device_v0_CreateForceFeedbackEffect( IGameInputDevice_v0 *iface, uint32_t index, const GameInputForceFeedbackParams *params,
                                                                  IGameInputForceFeedbackEffect_v0 **effect )
{
    FIXME( "device %p, index %u, params %p, effect %p stub!\n", device_from_IGameInputDevice_v0( iface ), index, params, effect );
    return E_NOTIMPL;
}

static bool WINAPI game_input_device_v0_IsForceFeedbackMotorPoweredOn( IGameInputDevice_v0 *iface, uint32_t index )
{
    FIXME( "device %p, index %u stub!\n", device_from_IGameInputDevice_v0( iface ), index );
    return FALSE;
}

static void WINAPI game_input_device_v0_SetForceFeedbackMotorGain( IGameInputDevice_v0 *iface, uint32_t index, float gain )
{
    FIXME( "device %p, index %u, gain %f stub!\n", device_from_IGameInputDevice_v0( iface ), index, gain );
}

static HRESULT WINAPI game_input_device_v0_SetHapticMotorState( IGameInputDevice_v0 *iface, uint32_t index, const GameInputHapticFeedbackParams *params )
{
    FIXME( "device %p, index %u, params %p stub!\n", device_from_IGameInputDevice_v0( iface ), index, params );
    return E_NOTIMPL;
}

static void WINAPI game_input_device_v0_SetRumbleState( IGameInputDevice_v0 *iface, const GameInputRumbleParams *params )
{
    FIXME( "device %p, params %p stub!\n", device_from_IGameInputDevice_v0( iface ), params );
}

static void WINAPI game_input_device_v0_SetInputSynchronizationState( IGameInputDevice_v0 *iface, bool enabled )
{
    FIXME( "device %p, enabled %d stub!\n", device_from_IGameInputDevice_v0( iface ), enabled );
}

static void WINAPI game_input_device_v0_SendInputSynchronizationHint( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
}

static void WINAPI game_input_device_v0_PowerOff( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
}

static HRESULT WINAPI game_input_device_v0_CreateRawDeviceReport( IGameInputDevice_v0 *iface, uint32_t report_id, GameInputRawDeviceReportKind report_kind,
                                                              IGameInputRawDeviceReport_v0 **report )
{
    FIXME( "device %p, report_id %u, report_kind %#x, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report_id, report_kind, report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_GetRawDeviceFeature( IGameInputDevice_v0 *iface, uint32_t report_id, IGameInputRawDeviceReport_v0 **report )
{
    FIXME( "device %p, report_id %u, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report_id, report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_SetRawDeviceFeature( IGameInputDevice_v0 *iface, IGameInputRawDeviceReport_v0 *report )
{
    FIXME( "device %p, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_SendRawDeviceOutput( IGameInputDevice_v0 *iface, IGameInputRawDeviceReport_v0 *report )
{
    FIXME( "device %p, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_SendRawDeviceOutputWithResponse( IGameInputDevice_v0 *iface, IGameInputRawDeviceReport_v0 *request_report,
                                                                        IGameInputRawDeviceReport_v0 **response_report )
{
    FIXME( "device %p, request_report %p, response_report %p stub!\n", device_from_IGameInputDevice_v0( iface ), request_report, response_report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_ExecuteRawDeviceIoControl( IGameInputDevice_v0 *iface, uint32_t control_code, size_t input_buffer_size, const void *input_buffer,
                                                                  size_t output_buffer_size, void *output_buffer, size_t *output_size )
{
    FIXME( "device %p, control_code %u, input_buffer_size %Iu, input_buffer %p, output_buffer_size %Iu, output_buffer %p, output_size %p stub!\n",
           device_from_IGameInputDevice_v0( iface ), control_code, input_buffer_size, input_buffer, output_buffer_size, output_buffer, output_size );
    return E_NOTIMPL;
}

static bool WINAPI game_input_device_v0_AcquireExclusiveRawDeviceAccess( IGameInputDevice_v0 *iface, uint64_t timeout_us )
{
    FIXME( "device %p, timeout_us %I64u stub!\n", device_from_IGameInputDevice_v0( iface ), timeout_us );
    return FALSE;
}

static void WINAPI game_input_device_v0_ReleaseExclusiveRawDeviceAccess( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
}

static const IGameInputDevice_v0Vtbl game_input_device_v0_vtbl =
{
    game_input_device_v0_QueryInterface,
    game_input_device_v0_AddRef,
    game_input_device_v0_Release,
    game_input_device_v0_GetDeviceInfo,
    game_input_device_v0_GetDeviceStatus,
    game_input_device_v0_GetBatteryState,
    game_input_device_v0_CreateForceFeedbackEffect,
    game_input_device_v0_IsForceFeedbackMotorPoweredOn,
    game_input_device_v0_SetForceFeedbackMotorGain,
    game_input_device_v0_SetHapticMotorState,
    game_input_device_v0_SetRumbleState,
    game_input_device_v0_SetInputSynchronizationState,
    game_input_device_v0_SendInputSynchronizationHint,
    game_input_device_v0_PowerOff,
    game_input_device_v0_CreateRawDeviceReport,
    game_input_device_v0_GetRawDeviceFeature,
    game_input_device_v0_SetRawDeviceFeature,
    game_input_device_v0_SendRawDeviceOutput,
    game_input_device_v0_SendRawDeviceOutputWithResponse,
    game_input_device_v0_ExecuteRawDeviceIoControl,
    game_input_device_v0_AcquireExclusiveRawDeviceAccess,
    game_input_device_v0_ReleaseExclusiveRawDeviceAccess,
};

static BOOL matches_device_interface( const DEV_OBJECT *object, const GUID *iid )
{
    for (UINT i = 0; i < object->cPropertyCount; i++)
    {
        const DEVPROPERTY *prop = object->pProperties + i;
        if (memcmp( &DEVPKEY_DeviceInterface_ClassGuid, &prop->CompKey.Key, sizeof(prop->CompKey.Key) )) continue;
        return IsEqualGUID( prop->Buffer, iid );
    }

    return FALSE;
}

static struct device *device_create( struct list *devices, const DEV_OBJECT *object )
{
    GameInputDeviceFamily family = GameInputFamilyHid;
    const WCHAR *device_path = object->pszObjectId;
    PHIDP_PREPARSED_DATA preparsed;
    struct device *device = NULL;
    HIDD_ATTRIBUTES attr;
    HIDP_CAPS caps;
    HANDLE file;
    WCHAR *tmp;

    if (!matches_device_interface( object, &GUID_DEVINTERFACE_HID ) &&
        !matches_device_interface( object, &GUID_DEVINTERFACE_WINEXINPUT ))
        return NULL;

    if ((tmp = wcschr( device_path + 8, '#' )) && !wcsnicmp( tmp - 6, L"&IG_", 4 )) return NULL;
    if (tmp && !wcsnicmp( tmp - 6, L"&XI_", 4 )) family = GameInputFamilyXbox360;

    TRACE( "device_path %s\n", debugstr_w( device_path ) );

    file = CreateFileW(device_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;

    HidD_GetAttributes( file, &attr );
    HidD_GetPreparsedData( file, &preparsed );
    HidP_GetCaps( preparsed, &caps );
    HidD_FreePreparsedData( preparsed );

    if (caps.UsagePage != HID_USAGE_PAGE_GENERIC || caps.Usage != HID_USAGE_GENERIC_GAMEPAD) goto done;
    /* CW-Bug-Id: #23185 Emulate Steam Input native hooks for native SDL */
    if (attr.VendorID == 0x28de && attr.ProductID == 0x11ff) goto done;

    if (!(device = calloc( 1, sizeof(*device) ))) goto done;
    device->IGameInputDevice_v0_iface.lpVtbl = &game_input_device_v0_vtbl;
    device->refcount = 1;
    wcscpy( device->path, device_path );

    device->info_v0.infoSize = sizeof(device->info_v0.infoSize);
    device->info_v0.vendorId = attr.VendorID;
    device->info_v0.productId = attr.ProductID;
    device->info_v0.usage.page = caps.UsagePage;
    device->info_v0.usage.id = caps.Usage;
    device->info_v0.deviceFamily = family;
    device->info_v0.capabilities = GameInputDeviceCapabilityNone;
    device->info_v0.supportedInput = GameInputKindUiNavigation_v0 | GameInputKindGamepad | GameInputKindController;
    device->info_v0.supportedRumbleMotors = GameInputRumbleNone;
    device->info_v0.controllerAxisCount = 6;
    device->info_v0.controllerButtonCount = caps.NumberInputButtonCaps;
    device->info_v0.controllerSwitchCount = 1;
    device->info_v0.supportedSystemButtons = GameInputSystemButtonGuide;

    list_add_tail( devices, &device->entry );

    TRACE( "created device %p\n", device );
done:
    CloseHandle( file );
    return device;
}

struct device_callback
{
    struct list entry;

    IGameInputDevice_v0 *device;
    GameInputKind input_kind;
    GameInputDeviceStatus status_filter;

    void *context;
    GameInputDeviceCallback_v0 callback;
};

static void device_callback_notify( struct device_callback *entry, IGameInputDevice_v0 *device, GameInputKind input_kind,
                                    GameInputDeviceStatus new_status, GameInputDeviceStatus old_status )
{
    if (entry->device && entry->device != device) return;
    if (!(entry->input_kind & input_kind)) return;
    if (!(entry->status_filter & (new_status | old_status))) return;
    entry->callback( (UINT_PTR)entry, entry->context, device, 0, new_status, old_status );
}

static HRESULT device_callback_create( IGameInputDevice_v0 *device, GameInputKind input_kind, GameInputDeviceStatus status_filter,
                                       void *context, GameInputDeviceCallback_v0 callback, struct device_callback **out )
{
    struct device_callback *entry;

    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    if ((entry->device = device)) IGameInputDevice_v0_AddRef( entry->device );
    entry->input_kind = input_kind;
    entry->status_filter = status_filter;
    entry->context = context;
    entry->callback = callback;

    *out = entry;
    return S_OK;
}

struct game_input
{
    IGameInput_v0 IGameInput_v0_iface;
    IGameInput_v2 IGameInput_v2_iface;
    LONG refcount;

    HDEVQUERY query;
    HANDLE initialized;
    struct list devices;
    struct list callbacks;
    struct list callbacks_v2;
};

/* A v2 device-callback registration; the token handed back is its address. */
struct device_callback_v2
{
    struct list entry;
    IGameInputDevice_v2 *device;
    GameInputKind kind;
    GameInputDeviceStatus status_filter;
    void *context;
    GameInputDeviceCallback_v2 callback;
};

static struct game_input *impl_from_v0_IGameInput( IGameInput_v0 *iface )
{
    return CONTAINING_RECORD( iface, struct game_input, IGameInput_v0_iface );
}

static HRESULT WINAPI game_input_v0_QueryInterface( IGameInput_v0 *iface, REFIID iid, void **out )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );

    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IGameInput_v0 ))
    {
        *out = &impl->IGameInput_v0_iface;
        IGameInput_v0_AddRef( &impl->IGameInput_v0_iface );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IGameInput_v2 ))
    {
        *out = &impl->IGameInput_v2_iface;
        IGameInput_v2_AddRef( &impl->IGameInput_v2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI game_input_v0_AddRef( IGameInput_v0 *iface )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );
    ULONG ref;

    EnterCriticalSection( &game_input_cs );
    ref = ++impl->refcount;
    LeaveCriticalSection( &game_input_cs );

    TRACE( "impl %p increasing refcount to %lu.\n", impl, ref );

    return ref;
}

static ULONG WINAPI game_input_v0_Release( IGameInput_v0 *iface )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );
    ULONG ref;

    EnterCriticalSection( &game_input_cs );
    if (!(ref = --impl->refcount)) game_input = NULL;
    LeaveCriticalSection( &game_input_cs );

    TRACE( "impl %p decreasing refcount to %lu.\n", impl, ref );

    if (!ref)
    {
        struct list *ptr;

        while ((ptr = list_head( &impl->callbacks )))
        {
            struct device_callback *callback = LIST_ENTRY(ptr, struct device_callback, entry);
            list_remove( &callback->entry );
            free( callback );
        }

        /* No longer reachable by the pad notification thread, but it may be
         * in the middle of delivering to one of these. */
        EnterCriticalSection( &device_callback_cs );
        LeaveCriticalSection( &device_callback_cs );
        while ((ptr = list_head( &impl->callbacks_v2 )))
        {
            struct device_callback_v2 *callback = LIST_ENTRY(ptr, struct device_callback_v2, entry);
            list_remove( &callback->entry );
            free( callback );
        }

        while ((ptr = list_head( &impl->devices )))
        {
            struct device *device = LIST_ENTRY(ptr, struct device, entry);
            list_remove( &device->entry );
            IGameInputDevice_v0_Release( &device->IGameInputDevice_v0_iface );
        }

        if (impl->query) DevCloseObjectQuery( impl->query );
        CloseHandle( impl->initialized );
        free( impl );
    }

    return ref;
}

static uint64_t WINAPI game_input_v0_GetCurrentTimestamp( IGameInput_v0 *iface )
{
    FIXME( "impl %p stub!\n", impl_from_v0_IGameInput( iface ) );
    return 0;
}

static HRESULT WINAPI game_input_v0_GetCurrentReading( IGameInput_v0 *iface, GameInputKind input_kind,
                                                       IGameInputDevice_v0 *device_iface, IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, input_kind %#x, device %p, reading %p\n", impl_from_v0_IGameInput( iface ), input_kind, device_iface, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_GetNextReading( IGameInput_v0 *iface, IGameInputReading_v0 *reference_reading, GameInputKind input_kind,
                                                    IGameInputDevice_v0 *device_iface, IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, reference_reading %p, input_kind %#x, device %p, reading %p\n", impl_from_v0_IGameInput( iface ), reference_reading, input_kind, device_iface, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_GetPreviousReading( IGameInput_v0 *iface, IGameInputReading_v0 *reference_reading,
                                                        GameInputKind input_kind, IGameInputDevice_v0 *device,
                                                        IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, reference_reading %p, input_kind %#x, device %p, reading %p stub!\n",
           impl_from_v0_IGameInput( iface ), reference_reading, input_kind, device, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_GetTemporalReading( IGameInput_v0 *iface, uint64_t timestamp, IGameInputDevice_v0 *device, IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, timestamp %I64u, device %p, reading %p stub!\n", impl_from_v0_IGameInput( iface ), timestamp, device, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_RegisterReadingCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device, GameInputKind input_kind, float analog_threshold,
                                                             void *context, GameInputReadingCallback_v0 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, input_kind %#x, analog_threshold %f, context %p, callback %p, token %p stub!\n",
           impl_from_v0_IGameInput( iface ), device, input_kind, analog_threshold, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_RegisterDeviceCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device_iface, GameInputKind input_kind, GameInputDeviceStatus status_filter,
                                                            GameInputEnumerationKind enumeration_kind, void *context, GameInputDeviceCallback_v0 callback,
                                                            GameInputCallbackToken *token )
{
    struct game_input *impl = CONTAINING_RECORD( iface, struct game_input, IGameInput_v0_iface );
    struct device_callback *entry;
    struct device *device;
    HRESULT hr;

    FIXME( "impl %p device_iface %p input_kind %#x status_filter %#x enumeration_kind %#x, context %p callback %p token %p stub!\n",
           impl, device_iface, input_kind, status_filter, enumeration_kind, context, callback, token );

    if (FAILED(hr = device_callback_create( device_iface, input_kind, status_filter, context, callback, &entry ))) return hr;

    EnterCriticalSection( &game_input_cs );
    list_add_tail( &impl->callbacks, &entry->entry );

    LIST_FOR_EACH_ENTRY( device, &impl->devices, struct device, entry )
        device_callback_notify( entry, &device->IGameInputDevice_v0_iface, GameInputKindGamepad,
                                device->status, GameInputDeviceNoStatus );

    LeaveCriticalSection( &game_input_cs );

    *token = (UINT_PTR)entry;

    return hr;
}

static HRESULT WINAPI game_input_v0_RegisterSystemButtonCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device,
                                                                  GameInputSystemButtons button_filter, void *context,
                                                                  GameInputSystemButtonCallback_v0 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, button_filter %#x, context %p, callback %p, token %p stub!\n",
           impl_from_v0_IGameInput( iface ), device, button_filter, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_RegisterKeyboardLayoutCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device, void *context,
                                                                    GameInputKeyboardLayoutCallback_v0 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, context %p, callback %p, token %p stub!\n",
           impl_from_v0_IGameInput( iface ), device, context, callback, token );
    return E_NOTIMPL;
}

static void WINAPI game_input_v0_StopCallback( IGameInput_v0 *iface, GameInputCallbackToken token )
{
    FIXME( "impl %p, token %I64u stub!\n", impl_from_v0_IGameInput( iface ), token );
}

static bool WINAPI game_input_v0_UnregisterCallback( IGameInput_v0 *iface, GameInputCallbackToken token, uint64_t timeout_us )
{
    FIXME( "impl %p, token %I64u, timeout_us %I64u stub!\n", impl_from_v0_IGameInput( iface ), token, timeout_us );
    return FALSE;
}

static HRESULT WINAPI game_input_v0_CreateDispatcher( IGameInput_v0 *iface, IGameInputDispatcher **dispatcher )
{
    FIXME( "impl %p, dispatcher %p stub!\n", impl_from_v0_IGameInput( iface ), dispatcher );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_CreateAggregateDevice( IGameInput_v0 *iface, GameInputKind input_kind, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, input_kind %#x, device %p stub!\n", impl_from_v0_IGameInput( iface ), input_kind, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromId( IGameInput_v0 *iface, const APP_LOCAL_DEVICE_ID *value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %p, device %p stub!\n", impl_from_v0_IGameInput( iface ), value, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromObject( IGameInput_v0 *iface, IUnknown *value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %p, device %p stub!\n", impl_from_v0_IGameInput( iface ), value, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromPlatformHandle( IGameInput_v0 *iface, HANDLE value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %p, device %p stub!\n", impl_from_v0_IGameInput( iface ), value, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromPlatformString( IGameInput_v0 *iface, const WCHAR *value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %s, device %p stub!\n", impl_from_v0_IGameInput( iface ), debugstr_w(value), device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_EnableOemDeviceSupport( IGameInput_v0 *iface, uint16_t vendor_id, uint16_t product_id,
                                                            uint8_t interface_number, uint8_t collection_number )
{
    FIXME( "impl %p, vendor_id %u, product_id %u, interface_number %u, collection_number %u stub!\n",
           impl_from_v0_IGameInput( iface ), vendor_id, product_id, interface_number, collection_number );
    return E_NOTIMPL;
}

static void WINAPI game_input_v0_SetFocusPolicy( IGameInput_v0 *iface, GameInputFocusPolicy policy )
{
    FIXME( "impl %p, policy %#x stub!\n", impl_from_v0_IGameInput( iface ), policy );
}

static const IGameInput_v0Vtbl game_input_v0_vtbl =
{
    game_input_v0_QueryInterface,
    game_input_v0_AddRef,
    game_input_v0_Release,
    game_input_v0_GetCurrentTimestamp,
    game_input_v0_GetCurrentReading,
    game_input_v0_GetNextReading,
    game_input_v0_GetPreviousReading,
    game_input_v0_GetTemporalReading,
    game_input_v0_RegisterReadingCallback,
    game_input_v0_RegisterDeviceCallback,
    game_input_v0_RegisterSystemButtonCallback,
    game_input_v0_RegisterKeyboardLayoutCallback,
    game_input_v0_StopCallback,
    game_input_v0_UnregisterCallback,
    game_input_v0_CreateDispatcher,
    game_input_v0_CreateAggregateDevice,
    game_input_v0_FindDeviceFromId,
    game_input_v0_FindDeviceFromObject,
    game_input_v0_FindDeviceFromPlatformHandle,
    game_input_v0_FindDeviceFromPlatformString,
    game_input_v0_EnableOemDeviceSupport,
    game_input_v0_SetFocusPolicy,
};

/* IGameInputDispatcher -- a title creates one to pump GameInput callbacks on
 * its own thread / task queue instead of GameInput's background thread. With no
 * devices there is never any work, so Dispatch reports "nothing pending" and the
 * wait handle stays unsignalled; creating it successfully is what a title needs
 * to finish wiring up its input task queue. */
struct game_input_dispatcher
{
    IGameInputDispatcher IGameInputDispatcher_iface;
    LONG refcount;
    HANDLE wait;
};

static struct game_input_dispatcher *impl_from_IGameInputDispatcher( IGameInputDispatcher *iface )
{
    return CONTAINING_RECORD( iface, struct game_input_dispatcher, IGameInputDispatcher_iface );
}

static HRESULT WINAPI dispatcher_QueryInterface( IGameInputDispatcher *iface, REFIID iid, void **out )
{
    struct game_input_dispatcher *impl = impl_from_IGameInputDispatcher( iface );

    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IGameInputDispatcher ))
    {
        *out = &impl->IGameInputDispatcher_iface;
        IGameInputDispatcher_AddRef( &impl->IGameInputDispatcher_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dispatcher_AddRef( IGameInputDispatcher *iface )
{
    struct game_input_dispatcher *impl = impl_from_IGameInputDispatcher( iface );
    return InterlockedIncrement( &impl->refcount );
}

static ULONG WINAPI dispatcher_Release( IGameInputDispatcher *iface )
{
    struct game_input_dispatcher *impl = impl_from_IGameInputDispatcher( iface );
    ULONG ref = InterlockedDecrement( &impl->refcount );
    if (!ref)
    {
        if (impl->wait) CloseHandle( impl->wait );
        free( impl );
    }
    return ref;
}

static bool WINAPI dispatcher_Dispatch( IGameInputDispatcher *iface, uint64_t quota_ms )
{
    TRACE( "impl %p, quota_ms %I64u.\n", impl_from_IGameInputDispatcher( iface ), quota_ms );
    return false; /* no queued work */
}

static HRESULT WINAPI dispatcher_OpenWaitHandle( IGameInputDispatcher *iface, HANDLE *handle )
{
    struct game_input_dispatcher *impl = impl_from_IGameInputDispatcher( iface );

    TRACE( "impl %p, handle %p.\n", impl, handle );

    if (!handle) return E_POINTER;
    if (!impl->wait && !(impl->wait = CreateEventW( NULL, TRUE, FALSE, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );
    *handle = impl->wait;
    return S_OK;
}

static const IGameInputDispatcherVtbl dispatcher_vtbl =
{
    dispatcher_QueryInterface,
    dispatcher_AddRef,
    dispatcher_Release,
    dispatcher_Dispatch,
    dispatcher_OpenWaitHandle,
};

static HRESULT create_dispatcher( IGameInputDispatcher **out )
{
    struct game_input_dispatcher *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IGameInputDispatcher_iface.lpVtbl = &dispatcher_vtbl;
    impl->refcount = 1;
    *out = &impl->IGameInputDispatcher_iface;
    return S_OK;
}

/* --- Virtual keyboard and mouse -------------------------------------------
 * With device enumeration off there would be no devices at all, and a title
 * that initialises its input through GameInput waits for a keyboard and mouse
 * to be enumerated before it leaves its loading screen (Minecraft sits at 41%
 * forever otherwise). Present one of each as always-connected devices of the
 * built-in (I8042) family. XInput gamepads use the same v2 device interface. */
struct virtual_device
{
    IGameInputDevice_v2 IGameInputDevice_v2_iface;
    LONG refcount;
    GameInputKind kind;
    GameInputDeviceInfo_v2 info;
    GameInputKeyboardInfo_v2 keyboard;
    GameInputMouseInfo_v2 mouse;
    GameInputGamepadInfo_v2 gamepad;
    DWORD xinput_index;
};

static struct virtual_device *impl_from_IGameInputDevice_v2( IGameInputDevice_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct virtual_device, IGameInputDevice_v2_iface );
}

static HRESULT WINAPI virtual_device_QueryInterface( IGameInputDevice_v2 *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IGameInputDevice_v2 ))
    {
        *out = iface;
        IGameInputDevice_v2_AddRef( iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI virtual_device_AddRef( IGameInputDevice_v2 *iface )
{
    return InterlockedIncrement( &impl_from_IGameInputDevice_v2( iface )->refcount );
}

static ULONG WINAPI virtual_device_Release( IGameInputDevice_v2 *iface )
{
    /* static objects, never freed */
    return InterlockedDecrement( &impl_from_IGameInputDevice_v2( iface )->refcount );
}

static HRESULT WINAPI virtual_device_GetDeviceInfo( IGameInputDevice_v2 *iface, const GameInputDeviceInfo_v2 **info )
{
    struct virtual_device *device = impl_from_IGameInputDevice_v2( iface );
    TRACE( "iface %p, info %p.\n", iface, info );
    if (!info) return E_POINTER;
    *info = &device->info;
    return S_OK;
}

static HRESULT WINAPI virtual_device_GetHapticInfo( IGameInputDevice_v2 *iface, GameInputHapticInfo *info )
{
    TRACE( "iface %p, info %p.\n", iface, info );
    return E_NOTIMPL;
}

static GameInputDeviceStatus WINAPI virtual_device_GetDeviceStatus( IGameInputDevice_v2 *iface )
{
    struct virtual_device *device = impl_from_IGameInputDevice_v2( iface );
    XINPUT_STATE state;

    TRACE( "iface %p.\n", iface );
    if (device->kind == GameInputKindGamepad && XInputGetState( device->xinput_index, &state ))
        return GameInputDeviceNoStatus;
    return GameInputDeviceConnected | GameInputDeviceInputEnabled;
}

static HRESULT WINAPI virtual_device_CreateForceFeedbackEffect( IGameInputDevice_v2 *iface, uint32_t motor,
                                                                const GameInputForceFeedbackParams *params,
                                                                IGameInputForceFeedbackEffect_v2 **effect )
{
    TRACE( "iface %p, motor %u, params %p, effect %p.\n", iface, motor, params, effect );
    if (effect) *effect = NULL;
    return E_NOTIMPL;
}

static bool WINAPI virtual_device_IsForceFeedbackMotorPoweredOn( IGameInputDevice_v2 *iface, uint32_t motor )
{
    return FALSE;
}

static void WINAPI virtual_device_SetForceFeedbackMotorGain( IGameInputDevice_v2 *iface, uint32_t motor, float gain )
{
}

static void WINAPI virtual_device_SetRumbleState( IGameInputDevice_v2 *iface, const GameInputRumbleParams *params )
{
    struct virtual_device *device = impl_from_IGameInputDevice_v2( iface );
    XINPUT_VIBRATION vibration = {0};

    if (device->kind != GameInputKindGamepad) return;
    if (params)
    {
        vibration.wLeftMotorSpeed = max( 0.0f, min( 1.0f, params->lowFrequency ) ) * 65535;
        vibration.wRightMotorSpeed = max( 0.0f, min( 1.0f, params->highFrequency ) ) * 65535;
    }
    XInputSetState( device->xinput_index, &vibration );
}

static HRESULT WINAPI virtual_device_DirectInputEscape( IGameInputDevice_v2 *iface, uint32_t command, const void *input,
                                                        uint32_t in_size, void *output, uint32_t out_size, uint32_t *size )
{
    TRACE( "iface %p, command %u.\n", iface, command );
    if (size) *size = 0;
    return E_NOTIMPL;
}

static const IGameInputDevice_v2Vtbl virtual_device_vtbl =
{
    virtual_device_QueryInterface,
    virtual_device_AddRef,
    virtual_device_Release,
    virtual_device_GetDeviceInfo,
    virtual_device_GetHapticInfo,
    virtual_device_GetDeviceStatus,
    virtual_device_CreateForceFeedbackEffect,
    virtual_device_IsForceFeedbackMotorPoweredOn,
    virtual_device_SetForceFeedbackMotorGain,
    virtual_device_SetRumbleState,
    virtual_device_DirectInputEscape,
};

static struct virtual_device virtual_keyboard = { {&virtual_device_vtbl}, 1, GameInputKindKeyboard };
static struct virtual_device virtual_mouse    = { {&virtual_device_vtbl}, 1, GameInputKindMouse };
/* Read pads through Wine's XInput rather than enumerating a second copy of each
 * through DevQuery. XInput only sees the pads winebus exposes through SDL or
 * evdev, Steam Input's virtual Xbox pad among them. A pad winebus hands to
 * hidraw instead (Sony's, unless told otherwise) reaches no XInput slot and is
 * not seen here either. PROTON_DISABLE_HIDRAW for its IDs moves it to SDL;
 * Ferestre's launcher sets that for Sony pads by default. */
static struct virtual_device virtual_gamepads[XUSER_MAX_COUNT] =
{
    { {&virtual_device_vtbl}, 1, GameInputKindGamepad },
    { {&virtual_device_vtbl}, 1, GameInputKindGamepad },
    { {&virtual_device_vtbl}, 1, GameInputKindGamepad },
    { {&virtual_device_vtbl}, 1, GameInputKindGamepad },
};
static struct virtual_device *virtual_devices[] =
{
    &virtual_keyboard, &virtual_mouse,
    &virtual_gamepads[0], &virtual_gamepads[1], &virtual_gamepads[2], &virtual_gamepads[3],
};

static void virtual_device_init( struct virtual_device *device, const char *name, BYTE id_byte, uint16_t usage_id )
{
    GameInputDeviceInfo_v2 *info = &device->info;

    memset( info, 0, sizeof(*info) );
    memset( info->deviceId.value, id_byte, sizeof(info->deviceId.value) );
    memset( info->deviceRootId.value, id_byte, sizeof(info->deviceRootId.value) );
    info->usage.page = HID_USAGE_PAGE_GENERIC;
    info->usage.id = usage_id;
    info->deviceFamily = GameInputFamilyI8042;
    info->supportedInput = device->kind;
    info->supportedRumbleMotors = GameInputRumbleNone;
    info->displayName = name;

    if (device->kind == GameInputKindKeyboard)
    {
        device->keyboard.kind = GameInputAnsiKeyboard;
        device->keyboard.layout = 0;
        device->keyboard.keyCount = 104;
        device->keyboard.functionKeyCount = 12;
        device->keyboard.maxSimultaneousKeys = 6;
        info->keyboardInfo = &device->keyboard;
    }
    else if (device->kind == GameInputKindMouse)
    {
        device->mouse.supportedButtons = GameInputMouseLeftButton | GameInputMouseRightButton | GameInputMouseMiddleButton |
                                         GameInputMouseButton4 | GameInputMouseButton5;
        device->mouse.sampleRate = 1000;
        device->mouse.hasWheelX = TRUE;
        device->mouse.hasWheelY = TRUE;
        info->mouseInfo = &device->mouse;
    }
}

static void virtual_devices_init( void )
{
    static BOOL done;
    unsigned int i;

    if (done) return;
    virtual_device_init( &virtual_keyboard, "Keyboard", 0x4b, HID_USAGE_GENERIC_KEYBOARD );
    virtual_device_init( &virtual_mouse, "Mouse", 0x4d, HID_USAGE_GENERIC_MOUSE );
    for (i = 0; i < ARRAY_SIZE(virtual_gamepads); i++)
    {
        struct virtual_device *device = &virtual_gamepads[i];
        virtual_device_init( device, "XInput Gamepad", 0x50 + i, HID_USAGE_GENERIC_GAMEPAD );
        device->xinput_index = i;
        device->info.deviceFamily = GameInputFamilyXbox360;
        /* What Wine's XInput presents every pad as: an Xbox 360 controller. */
        device->info.vendorId = 0x045e;
        device->info.productId = 0x028e;
        device->info.gamepadInfo = &device->gamepad;
        device->gamepad.menuButtonLabel = GameInputLabelXboxMenu;
        device->gamepad.viewButtonLabel = GameInputLabelXboxView;
        device->gamepad.aButtonLabel = GameInputLabelXboxA;
        device->gamepad.bButtonLabel = GameInputLabelXboxB;
        device->gamepad.xButtonLabel = GameInputLabelXboxX;
        device->gamepad.yButtonLabel = GameInputLabelXboxY;
        device->gamepad.dpadUpLabel = GameInputLabelXboxDPadUp;
        device->gamepad.dpadDownLabel = GameInputLabelXboxDPadDown;
        device->gamepad.dpadLeftLabel = GameInputLabelXboxDPadLeft;
        device->gamepad.dpadRightLabel = GameInputLabelXboxDPadRight;
        device->gamepad.leftShoulderButtonLabel = GameInputLabelXboxLeftShoulder;
        device->gamepad.rightShoulderButtonLabel = GameInputLabelXboxRightShoulder;
        device->gamepad.leftThumbstickButtonLabel = GameInputLabelXboxLeftStickButton;
        device->gamepad.rightThumbstickButtonLabel = GameInputLabelXboxRightStickButton;
        device->info.supportedRumbleMotors = GameInputRumbleLowFrequency | GameInputRumbleHighFrequency;
    }
    done = TRUE;
}

/* GameInput timestamps are microseconds from an arbitrary, monotonic base. The
 * performance counter is that base on Windows, and it is finer than the tick
 * count, which cannot tell apart two pad packets less than a millisecond apart.
 * Every timestamp here comes from this, so a reading's age can be taken against
 * GetCurrentTimestamp. */
static uint64_t gameinput_timestamp( void )
{
    LARGE_INTEGER counter, frequency;

    QueryPerformanceFrequency( &frequency );
    QueryPerformanceCounter( &counter );
    return counter.QuadPart / frequency.QuadPart * 1000000
           + counter.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart;
}

/* A reading for a virtual device. The keyboard and mouse have one static
 * reading each, which reads the live state whenever it is asked and is always
 * timestamped "now". A gamepad reading is a snapshot of one XInput packet,
 * timestamped when that packet was first seen (see gamepad_poll) and ordered
 * against every other pad's packets by its sequence number. */
struct virtual_reading
{
    IGameInputReading_v2 IGameInputReading_v2_iface;
    LONG refcount;
    struct virtual_device *device;
    XINPUT_STATE xinput;
    uint64_t timestamp;
    uint64_t sequence;
};

static struct virtual_reading *impl_from_IGameInputReading_v2( IGameInputReading_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct virtual_reading, IGameInputReading_v2_iface );
}

static HRESULT WINAPI virtual_reading_QueryInterface( IGameInputReading_v2 *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IGameInputReading_v2 ))
    {
        *out = iface;
        IGameInputReading_v2_AddRef( iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI virtual_reading_AddRef( IGameInputReading_v2 *iface )
{
    return InterlockedIncrement( &impl_from_IGameInputReading_v2( iface )->refcount );
}

static ULONG WINAPI virtual_reading_Release( IGameInputReading_v2 *iface )
{
    struct virtual_reading *reading = impl_from_IGameInputReading_v2( iface );
    ULONG ref = InterlockedDecrement( &reading->refcount );
    if (!ref && reading->device->kind == GameInputKindGamepad) free( reading );
    return ref;
}

static GameInputKind WINAPI virtual_reading_GetInputKind( IGameInputReading_v2 *iface )
{
    return impl_from_IGameInputReading_v2( iface )->device->kind;
}

static uint64_t WINAPI virtual_reading_GetTimestamp( IGameInputReading_v2 *iface )
{
    struct virtual_reading *reading = impl_from_IGameInputReading_v2( iface );
    return reading->device->kind == GameInputKindGamepad ? reading->timestamp : gameinput_timestamp();
}

static void WINAPI virtual_reading_GetDevice( IGameInputReading_v2 *iface, IGameInputDevice_v2 **device )
{
    struct virtual_reading *reading = impl_from_IGameInputReading_v2( iface );
    if (!device) return;
    *device = &reading->device->IGameInputDevice_v2_iface;
    IGameInputDevice_v2_AddRef( *device );
}

static uint32_t WINAPI virtual_reading_GetControllerAxisCount( IGameInputReading_v2 *iface ) { return 0; }
static uint32_t WINAPI virtual_reading_GetControllerAxisState( IGameInputReading_v2 *iface, uint32_t count, float *state ) { return 0; }
static uint32_t WINAPI virtual_reading_GetControllerButtonCount( IGameInputReading_v2 *iface ) { return 0; }
static uint32_t WINAPI virtual_reading_GetControllerButtonState( IGameInputReading_v2 *iface, uint32_t count, bool *state ) { return 0; }
static uint32_t WINAPI virtual_reading_GetControllerSwitchCount( IGameInputReading_v2 *iface ) { return 0; }
static uint32_t WINAPI virtual_reading_GetControllerSwitchState( IGameInputReading_v2 *iface, uint32_t count, GameInputSwitchPosition *state ) { return 0; }
/* Relative mouse motion, via raw input.
 *
 * Cursor-position deltas are only good while the pointer moves freely. The
 * moment a title takes the pointer -- which Minecraft does on entering a world,
 * hiding it and holding it in place -- those deltas collapse to zero and
 * looking around stops working even though the menus were fine. Raw input
 * reports the motion the device actually produced regardless of where the
 * pointer sits, so accumulate it on a message-only window. The counters below
 * are handed out as they stand and never cleared -- see GetMouseState for why.
 * The wheel comes from the same place, since there is no way to poll it. */
static LONG64 raw_dx, raw_dy, raw_wheel;

static LRESULT CALLBACK raw_input_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    RAWINPUT input;
    UINT size = sizeof(input);

    if (msg != WM_INPUT) return DefWindowProcW( hwnd, msg, wparam, lparam );

    if (GetRawInputData( (HRAWINPUT)lparam, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER) ) != (UINT)-1 &&
        input.header.dwType == RIM_TYPEMOUSE)
    {
        if (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
        {
            /* Tablets, and pointers on some remote and Wayland setups, report a
             * screen coordinate instead of movement. Difference it against the
             * previous one so it still feeds the same counters; the first such
             * report only establishes the origin. Only this thread runs here,
             * so plain statics are enough. */
            static LONG last_abs_x, last_abs_y;
            static BOOL have_last_abs;

            if (have_last_abs)
            {
                InterlockedExchangeAdd64( &raw_dx, input.data.mouse.lLastX - last_abs_x );
                InterlockedExchangeAdd64( &raw_dy, input.data.mouse.lLastY - last_abs_y );
            }
            last_abs_x = input.data.mouse.lLastX;
            last_abs_y = input.data.mouse.lLastY;
            have_last_abs = TRUE;
        }
        else
        {
            InterlockedExchangeAdd64( &raw_dx, input.data.mouse.lLastX );
            InterlockedExchangeAdd64( &raw_dy, input.data.mouse.lLastY );
        }
        if (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
            InterlockedExchangeAdd64( &raw_wheel, (SHORT)input.data.mouse.usButtonData );
    }
    return 0;
}

/* The message-only window raw input is delivered to, once its thread is up. */
static HWND raw_input_hwnd;

static DWORD WINAPI raw_input_thread( void *arg )
{
    static const WCHAR class_name[] = L"xodus_gameinput_raw";
    WNDCLASSEXW cls = { .cbSize = sizeof(cls) };
    HANDLE ready = arg;
    HWND hwnd;
    MSG msg;

    cls.lpfnWndProc = raw_input_proc;
    cls.lpszClassName = class_name;
    cls.hInstance = GetModuleHandleW( NULL );
    RegisterClassExW( &cls );

    hwnd = CreateWindowExW( 0, class_name, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, cls.hInstance, NULL );
    if (!hwnd) WARN( "could not create the raw input window; mouse motion will be limited.\n" );
    raw_input_hwnd = hwnd;
    SetEvent( ready );
    if (!hwnd) return 0;

    while (GetMessageW( &msg, NULL, 0, 0 ) > 0)
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}

static BOOL WINAPI start_raw_input( INIT_ONCE *once, void *param, void **ctx )
{
    HANDLE ready, thread;

    if (!(ready = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        WARN( "could not start the raw input thread.\n" );
        return TRUE;
    }
    /* Wait for the window, so the first reading can already register it. The
     * thread signals whether or not it managed to create one. */
    if ((thread = CreateThread( NULL, 0, raw_input_thread, ready, 0, NULL )))
    {
        WaitForSingleObject( ready, INFINITE );
        CloseHandle( thread );
    }
    else WARN( "could not start the raw input thread.\n" );
    CloseHandle( ready );
    return TRUE;
}

static void ensure_raw_input( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, start_raw_input, NULL, NULL );
}

/* Whether the process's raw mouse registration is currently ours, taking it
 * first if nobody holds it.
 *
 * Raw input is registered per process, with one target per kind of device, so
 * GameInput shares the mouse registration with the title and with dinput.
 * Registering replaces whoever held it, and dinput takes it while it has a
 * mouse acquired and removes it (RIDEV_REMOVE) when it lets go. Taking it
 * unconditionally silenced a title's own WM_INPUT, and taking it only once left
 * GameInput's counters frozen for the rest of the session after dinput let go.
 * So it is taken only while nobody else holds it, and checked again on every
 * reading; while someone else does, GetMouseState falls back to cursor motion.
 * The check and the registration are two calls, so a title registering between
 * them can still lose to us once, until it registers again. */
static BOOL raw_input_claim( void )
{
    RAWINPUTDEVICE buffer[32], *devices = buffer, mouse;
    UINT count = ARRAY_SIZE(buffer), found, i;
    BOOL ours = FALSE, held = FALSE;

    ensure_raw_input();
    if (!raw_input_hwnd) return FALSE;

    found = GetRegisteredRawInputDevices( devices, &count, sizeof(*devices) );
    if (found == (UINT)-1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
        (devices = malloc( count * sizeof(*devices) )))
        found = GetRegisteredRawInputDevices( devices, &count, sizeof(*devices) );
    if (found == (UINT)-1)
    {
        if (devices != buffer) free( devices );
        return FALSE;
    }

    for (i = 0; i < found; i++)
    {
        if (devices[i].usUsagePage != HID_USAGE_PAGE_GENERIC || devices[i].usUsage != HID_USAGE_GENERIC_MOUSE) continue;
        held = TRUE;
        ours = devices[i].hwndTarget == raw_input_hwnd;
        break;
    }
    if (devices != buffer) free( devices );

    if (!held)
    {
        mouse.usUsagePage = HID_USAGE_PAGE_GENERIC;
        mouse.usUsage = HID_USAGE_GENERIC_MOUSE;
        /* INPUTSINK: keep receiving motion even when the game window is not the
         * foreground one, so a reading is never silently starved. */
        mouse.dwFlags = RIDEV_INPUTSINK;
        mouse.hwndTarget = raw_input_hwnd;
        if (!(ours = RegisterRawInputDevices( &mouse, 1, sizeof(mouse) )))
            WARN( "RegisterRawInputDevices failed (%lu).\n", GetLastError() );
        else TRACE( "registered the raw mouse to %p\n", raw_input_hwnd );
    }
    return ours;
}

/* Keys currently down, as virtual-key codes. A title reads the count first and
 * then asks for that many, so both walk the same range. */
static uint32_t collect_keys( uint32_t count, GameInputKeyState *state )
{
    uint32_t found = 0;
    int vk;

    for (vk = 1; vk < 256; vk++)
    {
        /* Skip the mouse buttons: they belong to the mouse reading. */
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
            continue;
        if (!(GetAsyncKeyState( vk ) & 0x8000)) continue;
        if (state)
        {
            if (found >= count) break;
            state[found].virtualKey = vk;
            state[found].scanCode = MapVirtualKeyW( vk, MAPVK_VK_TO_VSC );
            state[found].codePoint = MapVirtualKeyW( vk, MAPVK_VK_TO_CHAR ) & 0xffff;
            state[found].isDeadKey = FALSE;
        }
        found++;
    }
    return found;
}

static uint32_t WINAPI virtual_reading_GetKeyCount( IGameInputReading_v2 *iface )
{
    struct virtual_reading *reading = impl_from_IGameInputReading_v2( iface );
    if (reading->device->kind != GameInputKindKeyboard) return 0;
    return collect_keys( 0, NULL );
}

static uint32_t WINAPI virtual_reading_GetKeyState( IGameInputReading_v2 *iface, uint32_t count, GameInputKeyState *state )
{
    struct virtual_reading *reading = impl_from_IGameInputReading_v2( iface );
    if (reading->device->kind != GameInputKindKeyboard || !state) return 0;
    return collect_keys( count, state );
}

/* GameInput reports mouse motion as a running total rather than a per-reading
 * delta: positionX/positionY are the sum of every movement delta so far, and a
 * caller works out how far the mouse moved by subtracting the previous
 * reading's value from the current one. Handing out a delta here made the title
 * take a difference of differences, which flips sign about as often as it
 * matches the real movement -- that was the erratic look-around. So these
 * counters only ever grow, and are never cleared.
 *
 * The movement comes from raw input rather than from the cursor whenever it
 * can (see raw_input_claim). Once a title pins the pointer to look around
 * GetCursorPos stops changing, and its deltas are accelerated screen pixels
 * rather than device counts in any case. absolutePositionX/Y is the field that
 * genuinely wants a screen coordinate. */
static SRWLOCK cursor_lock = SRWLOCK_INIT;
static POINT last_cursor;
static BOOL have_last_cursor;

static bool WINAPI virtual_reading_GetMouseState( IGameInputReading_v2 *iface, GameInputMouseState_v2 *state )
{
    struct virtual_reading *reading = impl_from_IGameInputReading_v2( iface );
    BOOL raw, have_pos;
    POINT pos;

    if (!state || reading->device->kind != GameInputKindMouse) return FALSE;
    memset( state, 0, sizeof(*state) );

    raw = raw_input_claim();
    if ((have_pos = GetCursorPos( &pos )))
    {
        /* While someone else holds the raw mouse registration no raw motion
         * arrives, so feed the same counters from the cursor instead. That
         * stops working once the pointer is held in place, but a title (or
         * dinput) holding raw input is reading its own motion from it. The
         * position is tracked either way, so switching over adds no jump. */
        AcquireSRWLockExclusive( &cursor_lock );
        if (!raw && have_last_cursor)
        {
            InterlockedExchangeAdd64( &raw_dx, pos.x - last_cursor.x );
            InterlockedExchangeAdd64( &raw_dy, pos.y - last_cursor.y );
        }
        last_cursor = pos;
        have_last_cursor = TRUE;
        ReleaseSRWLockExclusive( &cursor_lock );
    }

    state->positions = GameInputMouseRelativePosition;
    state->positionX = InterlockedCompareExchange64( &raw_dx, 0, 0 );
    state->positionY = InterlockedCompareExchange64( &raw_dy, 0, 0 );
    state->wheelY = InterlockedCompareExchange64( &raw_wheel, 0, 0 );

    if (have_pos)
    {
        state->positions |= GameInputMouseAbsolutePosition;
        state->absolutePositionX = pos.x;
        state->absolutePositionY = pos.y;
    }

    if (GetAsyncKeyState( VK_LBUTTON )  & 0x8000) state->buttons |= GameInputMouseLeftButton;
    if (GetAsyncKeyState( VK_RBUTTON )  & 0x8000) state->buttons |= GameInputMouseRightButton;
    if (GetAsyncKeyState( VK_MBUTTON )  & 0x8000) state->buttons |= GameInputMouseMiddleButton;
    if (GetAsyncKeyState( VK_XBUTTON1 ) & 0x8000) state->buttons |= GameInputMouseButton4;
    if (GetAsyncKeyState( VK_XBUTTON2 ) & 0x8000) state->buttons |= GameInputMouseButton5;
    return TRUE;
}
static bool WINAPI virtual_reading_GetSensorsState( IGameInputReading_v2 *iface, GameInputSensorsState *state ) { return FALSE; }
static bool WINAPI virtual_reading_GetArcadeStickState( IGameInputReading_v2 *iface, GameInputArcadeStickState *state ) { return FALSE; }
static bool WINAPI virtual_reading_GetFlightStickState( IGameInputReading_v2 *iface, GameInputFlightStickState *state ) { return FALSE; }
static float gamepad_axis( SHORT value )
{
    return value < 0 ? value / 32768.0f : value / 32767.0f;
}

static bool WINAPI virtual_reading_GetGamepadState( IGameInputReading_v2 *iface, GameInputGamepadState *state )
{
    struct virtual_reading *reading = impl_from_IGameInputReading_v2( iface );
    const XINPUT_GAMEPAD *pad = &reading->xinput.Gamepad;
    static const struct { WORD xinput; GameInputGamepadButtons gameinput; } buttons[] =
    {
        {XINPUT_GAMEPAD_START, GameInputGamepadMenu}, {XINPUT_GAMEPAD_BACK, GameInputGamepadView},
        {XINPUT_GAMEPAD_A, GameInputGamepadA}, {XINPUT_GAMEPAD_B, GameInputGamepadB},
        {XINPUT_GAMEPAD_X, GameInputGamepadX}, {XINPUT_GAMEPAD_Y, GameInputGamepadY},
        {XINPUT_GAMEPAD_DPAD_UP, GameInputGamepadDPadUp}, {XINPUT_GAMEPAD_DPAD_DOWN, GameInputGamepadDPadDown},
        {XINPUT_GAMEPAD_DPAD_LEFT, GameInputGamepadDPadLeft}, {XINPUT_GAMEPAD_DPAD_RIGHT, GameInputGamepadDPadRight},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, GameInputGamepadLeftShoulder},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, GameInputGamepadRightShoulder},
        {XINPUT_GAMEPAD_LEFT_THUMB, GameInputGamepadLeftThumbstick},
        {XINPUT_GAMEPAD_RIGHT_THUMB, GameInputGamepadRightThumbstick},
    };
    unsigned int i;

    if (!state || reading->device->kind != GameInputKindGamepad) return FALSE;
    memset( state, 0, sizeof(*state) );
    for (i = 0; i < ARRAY_SIZE(buttons); i++)
        if (pad->wButtons & buttons[i].xinput) state->buttons |= buttons[i].gameinput;
    state->leftTrigger = pad->bLeftTrigger / 255.0f;
    state->rightTrigger = pad->bRightTrigger / 255.0f;
    state->leftThumbstickX = gamepad_axis( pad->sThumbLX );
    state->leftThumbstickY = gamepad_axis( pad->sThumbLY );
    state->rightThumbstickX = gamepad_axis( pad->sThumbRX );
    state->rightThumbstickY = gamepad_axis( pad->sThumbRY );
    return TRUE;
}
static bool WINAPI virtual_reading_GetRacingWheelState( IGameInputReading_v2 *iface, GameInputRacingWheelState *state ) { return FALSE; }
static bool WINAPI virtual_reading_GetUiNavigationState( IGameInputReading_v2 *iface, GameInputUiNavigationState *state ) { return FALSE; }

static const IGameInputReading_v2Vtbl virtual_reading_vtbl =
{
    virtual_reading_QueryInterface,
    virtual_reading_AddRef,
    virtual_reading_Release,
    virtual_reading_GetInputKind,
    virtual_reading_GetTimestamp,
    virtual_reading_GetDevice,
    virtual_reading_GetControllerAxisCount,
    virtual_reading_GetControllerAxisState,
    virtual_reading_GetControllerButtonCount,
    virtual_reading_GetControllerButtonState,
    virtual_reading_GetControllerSwitchCount,
    virtual_reading_GetControllerSwitchState,
    virtual_reading_GetKeyCount,
    virtual_reading_GetKeyState,
    virtual_reading_GetMouseState,
    virtual_reading_GetSensorsState,
    virtual_reading_GetArcadeStickState,
    virtual_reading_GetFlightStickState,
    virtual_reading_GetGamepadState,
    virtual_reading_GetRacingWheelState,
    virtual_reading_GetUiNavigationState,
};

static struct virtual_reading virtual_keyboard_reading = { {&virtual_reading_vtbl}, 1, &virtual_keyboard };
static struct virtual_reading virtual_mouse_reading    = { {&virtual_reading_vtbl}, 1, &virtual_mouse };

/* The current reading of the first virtual device matching kind/device, or NULL. */
static struct virtual_reading *virtual_reading_for( GameInputKind kind, IGameInputDevice_v2 *device )
{
    static struct virtual_reading *const readings[] = { &virtual_keyboard_reading, &virtual_mouse_reading };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(readings); i++)
    {
        struct virtual_reading *r = readings[i];
        if (!(r->device->kind & kind)) continue;
        if (device && device != &r->device->IGameInputDevice_v2_iface) continue;
        return r;
    }
    return NULL;
}

/* What each XInput slot last reported. XInput numbers its packets but does not
 * time them, so a packet is stamped when it is first polled and keeps that
 * stamp for as long as the pad stays idle: polling an untouched pad twice gives
 * the same timestamp, which is how titles tell new input from a repeat (and
 * which device was used last, for button prompts). The sequence number orders
 * those changes across every slot, which is what a reading for "any pad" is
 * chosen by. A pad's first sighting is not input, so it gets sequence 0 and an
 * idle pad never outranks one that has actually been used. Guarded by
 * game_input_cs. */
struct gamepad_slot
{
    BOOL seen;
    DWORD packet;
    uint64_t timestamp;
    uint64_t sequence;
};
static struct gamepad_slot gamepad_slots[XUSER_MAX_COUNT];
static uint64_t gamepad_sequence;

/* Poll one slot. FALSE if no pad is connected there; otherwise its state, and
 * when and in what order that state first appeared. */
static BOOL gamepad_poll( unsigned int index, XINPUT_STATE *state, struct gamepad_slot *out )
{
    struct gamepad_slot *slot = &gamepad_slots[index];
    BOOL connected;

    /* Read and recorded under one lock, so two threads polling the same pad
     * cannot record an older packet over a newer one. */
    EnterCriticalSection( &game_input_cs );
    if ((connected = !XInputGetState( index, state )))
    {
        if (!slot->seen || slot->packet != state->dwPacketNumber)
        {
            slot->sequence = slot->seen ? ++gamepad_sequence : 0;
            slot->seen = TRUE;
            slot->packet = state->dwPacketNumber;
            slot->timestamp = gameinput_timestamp();
        }
        *out = *slot;
    }
    else slot->seen = FALSE;
    LeaveCriticalSection( &game_input_cs );
    return connected;
}

/* Gamepad readings own a snapshot, so a later poll cannot change a reading
 * the title retained. The existing keyboard/mouse objects remain static.
 *
 * With a device, the reading is that pad's. Without one it may come from any
 * pad, as from GameInput's single input stream: the current reading is the
 * pad whose input changed last, and the next reading after a reference is the
 * pad whose input changed soonest after the reference was taken. Either way a
 * next reading exists only if something changed after the reference, which is
 * what ends a title's drain loop. */
static HRESULT gamepad_reading( GameInputKind kind, IGameInputDevice_v2 *device,
                                IGameInputReading_v2 *reference, IGameInputReading_v2 **out )
{
    struct virtual_reading *reading;
    struct gamepad_slot slot, best_slot = {0};
    XINPUT_STATE state, best_state = {0};
    BOOL after = FALSE, found = FALSE;
    uint64_t since = 0;
    unsigned int i, best = 0;

    if (!(kind & GameInputKindGamepad)) return GAMEINPUT_E_READING_NOT_FOUND;
    if (reference && reference->lpVtbl == &virtual_reading_vtbl)
    {
        after = TRUE;
        since = impl_from_IGameInputReading_v2( reference )->sequence;
    }
    for (i = 0; i < ARRAY_SIZE(virtual_gamepads); i++)
    {
        if (device && device != &virtual_gamepads[i].IGameInputDevice_v2_iface) continue;
        if (!gamepad_poll( i, &state, &slot )) continue;
        if (after && slot.sequence <= since) continue;
        /* Strict comparisons, so pads that are equally idle go to the lowest slot. */
        if (found && (after ? slot.sequence >= best_slot.sequence : slot.sequence <= best_slot.sequence)) continue;
        found = TRUE;
        best = i;
        best_state = state;
        best_slot = slot;
    }
    if (!found) return GAMEINPUT_E_READING_NOT_FOUND;

    if (!(reading = calloc( 1, sizeof(*reading) ))) return E_OUTOFMEMORY;
    reading->IGameInputReading_v2_iface.lpVtbl = &virtual_reading_vtbl;
    reading->refcount = 1;
    reading->device = &virtual_gamepads[best];
    reading->xinput = best_state;
    reading->timestamp = best_slot.timestamp;
    reading->sequence = best_slot.sequence;
    *out = &reading->IGameInputReading_v2_iface;
    return S_OK;
}

/* Device callbacks for pads that come and go.
 *
 * Registering a device callback reports what is connected at that moment, but
 * a title that keeps its pad list from these callbacks also has to hear about a
 * pad switched on after it registered, or one that drops out. XInput has no
 * notification for that, so while any callback that covers gamepads is
 * registered, a thread polls the slots and reports each change to every
 * matching callback, outside game_input_cs. hotplug_status is what those
 * callbacks were last told, and registering enumerates pads from it rather than
 * from XInput, so a change that lands during a registration reaches it exactly
 * once. Both are guarded by game_input_cs. */
#define HOTPLUG_INTERVAL_MS 100
static GameInputDeviceStatus hotplug_status[XUSER_MAX_COUNT];
static BOOL hotplug_running;

/* One callback to make. The entry is kept only to check, just before the call,
 * that it is still registered; what the call needs is copied out of it. */
struct device_notification
{
    struct device_callback_v2 *entry;
    GameInputDeviceCallback_v2 callback;
    void *context;
    struct virtual_device *device;
    GameInputDeviceStatus current;
    GameInputDeviceStatus previous;
};

static BOOL device_callback_matches( const struct device_callback_v2 *entry, const struct virtual_device *device )
{
    if (!(entry->kind & device->kind)) return FALSE;
    return !entry->device || entry->device == &device->IGameInputDevice_v2_iface;
}

static GameInputDeviceStatus gamepad_status( unsigned int index )
{
    XINPUT_STATE state;
    return XInputGetState( index, &state ) ? GameInputDeviceNoStatus : GameInputDeviceConnected;
}

/* Called with game_input_cs held. */
static BOOL device_callback_registered( const struct device_callback_v2 *entry )
{
    struct device_callback_v2 *cur;

    if (!game_input) return FALSE;
    LIST_FOR_EACH_ENTRY( cur, &game_input->callbacks_v2, struct device_callback_v2, entry )
        if (cur == entry) return TRUE;
    return FALSE;
}

/* Called with game_input_cs held. */
static BOOL hotplug_wanted( void )
{
    struct device_callback_v2 *entry;

    if (!game_input) return FALSE;
    LIST_FOR_EACH_ENTRY( entry, &game_input->callbacks_v2, struct device_callback_v2, entry )
        if (entry->kind & GameInputKindGamepad) return TRUE;
    return FALSE;
}

static void deliver_device_notifications( const struct device_notification *notes, unsigned int count )
{
    uint64_t timestamp = gameinput_timestamp();
    unsigned int i;
    BOOL registered;

    for (i = 0; i < count; i++)
    {
        /* An earlier callback in the same batch may have unregistered this one. */
        EnterCriticalSection( &game_input_cs );
        registered = device_callback_registered( notes[i].entry );
        LeaveCriticalSection( &game_input_cs );
        if (!registered) continue;
        TRACE( "reporting %s status %#x (was %#x)\n", notes[i].device->info.displayName,
               notes[i].current, notes[i].previous );
        notes[i].callback( (UINT_PTR)notes[i].entry, notes[i].context, &notes[i].device->IGameInputDevice_v2_iface,
                           timestamp, notes[i].current, notes[i].previous );
    }
}

/* Look for pads that came or went since the last poll and report them. Called
 * on the notification thread with device_callback_cs held. */
static void hotplug_poll( void )
{
    struct device_notification *notes = NULL, *grown;
    unsigned int count = 0, capacity = 0, size, i;
    struct device_callback_v2 *entry;

    EnterCriticalSection( &game_input_cs );
    for (i = 0; i < ARRAY_SIZE(virtual_gamepads); i++)
    {
        struct virtual_device *device = &virtual_gamepads[i];
        GameInputDeviceStatus current = gamepad_status( i ), previous = hotplug_status[i];

        if (current == previous) continue;
        hotplug_status[i] = current;
        TRACE( "XInput slot %u %s\n", i, current ? "connected" : "disconnected" );
        if (!game_input) continue;

        LIST_FOR_EACH_ENTRY( entry, &game_input->callbacks_v2, struct device_callback_v2, entry )
        {
            if (!device_callback_matches( entry, device ) || !(entry->status_filter & (current ^ previous))) continue;
            if (count == capacity)
            {
                size = max( 8, capacity * 2 );
                if (!(grown = realloc( notes, size * sizeof(*notes) )))
                {
                    ERR( "out of memory; a pad change goes unreported\n" );
                    break;
                }
                notes = grown;
                capacity = size;
            }
            notes[count].entry = entry;
            notes[count].callback = entry->callback;
            notes[count].context = entry->context;
            notes[count].device = device;
            notes[count].current = current;
            notes[count].previous = previous;
            count++;
        }
    }
    LeaveCriticalSection( &game_input_cs );

    deliver_device_notifications( notes, count );
    free( notes );
}

/* Runs while some device callback covers gamepads, and holds a reference to the
 * module so that it can outlive the title's last GameInput object. */
static DWORD WINAPI hotplug_thread( void *module )
{
    BOOL wanted;

    do
    {
        Sleep( HOTPLUG_INTERVAL_MS );
        EnterCriticalSection( &device_callback_cs );
        EnterCriticalSection( &game_input_cs );
        if (!(wanted = hotplug_wanted())) hotplug_running = FALSE;
        LeaveCriticalSection( &game_input_cs );
        if (wanted) hotplug_poll();
        LeaveCriticalSection( &device_callback_cs );
    } while (wanted);

    TRACE( "no device callback covers pads any more; stopping\n" );
    FreeLibraryAndExitThread( module, 0 );
    return 0;
}

/* Called with game_input_cs held, by a registration that covers gamepads. A
 * thread that is not running yet starts from what XInput reports now. */
static void hotplug_start( void )
{
    HMODULE module;
    HANDLE thread;
    unsigned int i;

    if (hotplug_running) return;
    for (i = 0; i < ARRAY_SIZE(hotplug_status); i++) hotplug_status[i] = gamepad_status( i );

    if (!GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (const WCHAR *)hotplug_thread, &module ))
    {
        WARN( "could not reference the module (%lu); pads connected later go unreported\n", GetLastError() );
        return;
    }
    if (!(thread = CreateThread( NULL, 0, hotplug_thread, module, 0, NULL )))
    {
        WARN( "could not start the pad notification thread (%lu)\n", GetLastError() );
        FreeLibrary( module );
        return;
    }
    CloseHandle( thread );
    hotplug_running = TRUE;
}

/* IGameInput_v2 -- the interface Minecraft (and other recent GDK titles) query
 * for. It shares one game_input object (and refcount) with the v0 interface;
 * the vtable differs, so it needs its own thunks. Keyboard/mouse readings use
 * raw input; connected gamepads are enumerated and read through XInput. */

static struct game_input *impl_from_v2_IGameInput( IGameInput_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct game_input, IGameInput_v2_iface );
}

static HRESULT WINAPI game_input_v2_QueryInterface( IGameInput_v2 *iface, REFIID iid, void **out )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );

    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IGameInput_v0 ))
    {
        *out = &impl->IGameInput_v0_iface;
        IGameInput_v0_AddRef( &impl->IGameInput_v0_iface );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IGameInput_v2 ))
    {
        *out = &impl->IGameInput_v2_iface;
        IGameInput_v2_AddRef( &impl->IGameInput_v2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI game_input_v2_AddRef( IGameInput_v2 *iface )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    return game_input_v0_AddRef( &impl->IGameInput_v0_iface );
}

static ULONG WINAPI game_input_v2_Release( IGameInput_v2 *iface )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    return game_input_v0_Release( &impl->IGameInput_v0_iface );
}

static uint64_t WINAPI game_input_v2_GetCurrentTimestamp( IGameInput_v2 *iface )
{
    return gameinput_timestamp();
}

static HRESULT WINAPI game_input_v2_GetCurrentReading( IGameInput_v2 *iface, GameInputKind kind,
                                                       IGameInputDevice_v2 *device, IGameInputReading_v2 **reading )
{
    struct virtual_reading *r;

    TRACE( "impl %p, kind %#x, device %p, reading %p.\n", impl_from_v2_IGameInput( iface ), kind, device, reading );

    if (!reading) return E_POINTER;
    *reading = NULL;
    if (kind & GameInputKindGamepad)
    {
        HRESULT hr = gamepad_reading( kind, device, NULL, reading );
        if (hr != GAMEINPUT_E_READING_NOT_FOUND) return hr;
    }
    if (!(r = virtual_reading_for( kind, device )))
    {
        *reading = NULL;
        return GAMEINPUT_E_READING_NOT_FOUND;
    }
    *reading = &r->IGameInputReading_v2_iface;
    IGameInputReading_v2_AddRef( *reading );
    return S_OK;
}

static HRESULT WINAPI game_input_v2_GetNextReading( IGameInput_v2 *iface, IGameInputReading_v2 *reference,
                                                    GameInputKind kind, IGameInputDevice_v2 *device, IGameInputReading_v2 **reading )
{
    TRACE( "impl %p, reference %p, kind %#x, device %p, reading %p.\n", impl_from_v2_IGameInput( iface ), reference, kind, device, reading );

    if (!reading) return E_POINTER;
    *reading = NULL;
    return gamepad_reading( kind, device, reference, reading );
}

static HRESULT WINAPI game_input_v2_GetPreviousReading( IGameInput_v2 *iface, IGameInputReading_v2 *reference,
                                                        GameInputKind kind, IGameInputDevice_v2 *device, IGameInputReading_v2 **reading )
{
    TRACE( "impl %p, reference %p, kind %#x, device %p, reading %p.\n", impl_from_v2_IGameInput( iface ), reference, kind, device, reading );

    if (!reading) return E_POINTER;
    *reading = NULL;
    return GAMEINPUT_E_READING_NOT_FOUND;
}

static HRESULT WINAPI game_input_v2_RegisterReadingCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device, GameInputKind kind,
                                                             void *context, GameInputReadingCallback_v2 callback, GameInputCallbackToken *token )
{
    TRACE( "impl %p, device %p, kind %#x, context %p, callback %p, token %p.\n", impl_from_v2_IGameInput( iface ), device, kind, context, callback, token );
    if (token) *token = 1;
    return S_OK;
}

static HRESULT WINAPI game_input_v2_RegisterDeviceCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device, GameInputKind kind,
                                                            GameInputDeviceStatus filter, GameInputEnumerationKind enum_kind, void *context,
                                                            GameInputDeviceCallback_v2 callback, GameInputCallbackToken *token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct device_notification initial[ARRAY_SIZE(virtual_devices)];
    struct device_callback_v2 *entry;
    unsigned int i, count = 0;

    TRACE( "impl %p, device %p, kind %#x, filter %#x, enum_kind %#x, context %p, callback %p, token %p.\n",
           impl, device, kind, filter, enum_kind, context, callback, token );

    if (!callback || !token) return E_INVALIDARG;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    entry->device = device;
    entry->kind = kind;
    entry->status_filter = filter;
    entry->context = context;
    entry->callback = callback;

    /* Held until the devices already connected have been reported, so the
     * pad notification thread cannot report a change to this callback first:
     * a pad that drops out while the keyboard and mouse are being reported
     * would otherwise be reported gone and then, from the stale list taken
     * here, connected -- and a title that keeps its pad list from these calls
     * would hold on to a pad that is not there. Same lock order as that
     * thread, and recursive, so a callback may still register or unregister. */
    EnterCriticalSection( &device_callback_cs );
    EnterCriticalSection( &game_input_cs );
    list_add_tail( &impl->callbacks_v2, &entry->entry );
    if (kind & GameInputKindGamepad) hotplug_start();

    /* Report what is already connected. Pads are taken from what the
     * notification thread last reported, so that whatever changes from here
     * on is reported by that thread, once, and after these. */
    if (enum_kind != GameInputNoEnumeration && (filter & GameInputDeviceConnected))
    {
        for (i = 0; i < ARRAY_SIZE(virtual_devices); i++)
        {
            struct virtual_device *vd = virtual_devices[i];
            if (!device_callback_matches( entry, vd )) continue;
            if (vd->kind == GameInputKindGamepad && !(hotplug_status[vd->xinput_index] & GameInputDeviceConnected)) continue;
            initial[count].entry = entry;
            initial[count].callback = callback;
            initial[count].context = context;
            initial[count].device = vd;
            initial[count].current = GameInputDeviceConnected;
            initial[count].previous = GameInputDeviceNoStatus;
            count++;
        }
    }
    LeaveCriticalSection( &game_input_cs );
    *token = (UINT_PTR)entry;

    /* Delivered synchronously, outside game_input_cs, which is what
     * BlockingEnumeration means; titles that ask for AsyncEnumeration accept
     * the same events. */
    deliver_device_notifications( initial, count );
    LeaveCriticalSection( &device_callback_cs );
    return S_OK;
}

static HRESULT WINAPI game_input_v2_RegisterSystemButtonCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device, GameInputSystemButtons filter,
                                                                  void *context, GameInputSystemButtonCallback_v2 callback, GameInputCallbackToken *token )
{
    TRACE( "impl %p, device %p, filter %#x, context %p, callback %p, token %p.\n", impl_from_v2_IGameInput( iface ), device, filter, context, callback, token );
    if (token) *token = 1;
    return S_OK;
}

static HRESULT WINAPI game_input_v2_RegisterKeyboardLayoutCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device, void *context,
                                                                    GameInputKeyboardLayoutCallback_v2 callback, GameInputCallbackToken *token )
{
    TRACE( "impl %p, device %p, context %p, callback %p, token %p.\n", impl_from_v2_IGameInput( iface ), device, context, callback, token );
    if (token) *token = 1;
    return S_OK;
}

static void WINAPI game_input_v2_StopCallback( IGameInput_v2 *iface, GameInputCallbackToken token )
{
    TRACE( "impl %p, token %I64u.\n", impl_from_v2_IGameInput( iface ), token );
}

static bool WINAPI game_input_v2_UnregisterCallback( IGameInput_v2 *iface, GameInputCallbackToken token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct device_callback_v2 *entry;

    TRACE( "impl %p, token %I64u.\n", impl, token );

    EnterCriticalSection( &game_input_cs );
    LIST_FOR_EACH_ENTRY( entry, &impl->callbacks_v2, struct device_callback_v2, entry )
    {
        if ((UINT_PTR)entry != token) continue;
        list_remove( &entry->entry );
        free( entry );
        break;
    }
    LeaveCriticalSection( &game_input_cs );

    /* Return only once a delivery in progress on the pad notification thread
     * is over, so that the title may free what the callback's context points
     * to. A callback unregistering itself holds this already. */
    EnterCriticalSection( &device_callback_cs );
    LeaveCriticalSection( &device_callback_cs );
    return TRUE;
}

static HRESULT WINAPI game_input_v2_CreateDispatcher( IGameInput_v2 *iface, IGameInputDispatcher **dispatcher )
{
    TRACE( "impl %p, dispatcher %p.\n", impl_from_v2_IGameInput( iface ), dispatcher );
    return create_dispatcher( dispatcher );
}

static HRESULT WINAPI game_input_v2_FindDeviceFromId( IGameInput_v2 *iface, const APP_LOCAL_DEVICE_ID *value, IGameInputDevice_v2 **device )
{
    unsigned int i;

    TRACE( "impl %p, value %p, device %p.\n", impl_from_v2_IGameInput( iface ), value, device );

    if (!device) return E_POINTER;
    *device = NULL;
    if (!value) return E_INVALIDARG;
    for (i = 0; i < ARRAY_SIZE(virtual_devices); i++)
    {
        struct virtual_device *vd = virtual_devices[i];
        if (memcmp( value->value, vd->info.deviceId.value, sizeof(value->value) )) continue;
        *device = &vd->IGameInputDevice_v2_iface;
        IGameInputDevice_v2_AddRef( *device );
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_FindDeviceFromPlatformString( IGameInput_v2 *iface, const WCHAR *value, IGameInputDevice_v2 **device )
{
    TRACE( "impl %p, value %s, device %p.\n", impl_from_v2_IGameInput( iface ), debugstr_w(value), device );
    if (device) *device = NULL;
    return E_NOTIMPL;
}

static void WINAPI game_input_v2_SetFocusPolicy( IGameInput_v2 *iface, GameInputFocusPolicy policy )
{
    TRACE( "impl %p, policy %#x.\n", impl_from_v2_IGameInput( iface ), policy );
}

static const IGameInput_v2Vtbl game_input_v2_vtbl =
{
    game_input_v2_QueryInterface,
    game_input_v2_AddRef,
    game_input_v2_Release,
    game_input_v2_GetCurrentTimestamp,
    game_input_v2_GetCurrentReading,
    game_input_v2_GetNextReading,
    game_input_v2_GetPreviousReading,
    game_input_v2_RegisterReadingCallback,
    game_input_v2_RegisterDeviceCallback,
    game_input_v2_RegisterSystemButtonCallback,
    game_input_v2_RegisterKeyboardLayoutCallback,
    game_input_v2_StopCallback,
    game_input_v2_UnregisterCallback,
    game_input_v2_CreateDispatcher,
    game_input_v2_FindDeviceFromId,
    game_input_v2_FindDeviceFromPlatformString,
    game_input_v2_SetFocusPolicy,
};

static struct device *find_device( struct list *devices, const DEV_OBJECT *object )
{
    struct device *device;

    LIST_FOR_EACH_ENTRY( device, devices, struct device, entry )
        if (!wcscmp( device->path, object->pszObjectId )) return device;
    return NULL;
}

static void WINAPI device_query_cb( HDEVQUERY devquery, void *context, const DEV_QUERY_RESULT_ACTION_DATA *action )
{
    struct game_input *impl = context;
    struct device *device;

    switch (action->Action)
    {
    case DevQueryResultStateChange:
        TRACE( "impl %p, DevQueryResultStateChange %u\n", impl, action->Data.State );
        SetEvent( impl->initialized );
        break;

    case DevQueryResultAdd:
        TRACE( "impl %p, action %u type %u id %s props %lu\n", impl, action->Action, action->Data.DeviceObject.ObjectType,
               debugstr_w(action->Data.DeviceObject.pszObjectId), action->Data.DeviceObject.cPropertyCount );

        EnterCriticalSection( &game_input_cs );
        if ((device = find_device( &impl->devices, &action->Data.DeviceObject )) ||
            (device = device_create( &impl->devices, &action->Data.DeviceObject )))
        {
            GameInputDeviceStatus old_status = device->status;
            struct device_callback *entry;
            device->status = GameInputDeviceConnected;

            LIST_FOR_EACH_ENTRY( entry, &impl->callbacks, struct device_callback, entry )
                device_callback_notify( entry, &device->IGameInputDevice_v0_iface, GameInputKindGamepad,
                                        device->status, old_status );
        }
        LeaveCriticalSection( &game_input_cs );

        break;

    case DevQueryResultRemove:
        TRACE( "impl %p, action %u type %u id %s props %lu\n", impl, action->Action, action->Data.DeviceObject.ObjectType,
               debugstr_w(action->Data.DeviceObject.pszObjectId), action->Data.DeviceObject.cPropertyCount );

        EnterCriticalSection( &game_input_cs );
        if ((device = find_device( &impl->devices, &action->Data.DeviceObject )))
        {
            GameInputDeviceStatus old_status = device->status;
            struct device_callback *entry;
            device->status = GameInputDeviceNoStatus;

            LIST_FOR_EACH_ENTRY( entry, &impl->callbacks, struct device_callback, entry )
                device_callback_notify( entry, &device->IGameInputDevice_v0_iface, GameInputKindGamepad,
                                        device->status, old_status );
        }
        LeaveCriticalSection( &game_input_cs );

        break;

    case DevQueryResultUpdate:
        TRACE( "impl %p, action %u type %u id %s props %lu\n", impl, action->Action, action->Data.DeviceObject.ObjectType,
               debugstr_w(action->Data.DeviceObject.pszObjectId), action->Data.DeviceObject.cPropertyCount );
        break;
    }
}

static struct game_input *game_input_create( BOOL devquery )
{
    struct game_input *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IGameInput_v0_iface.lpVtbl = &game_input_v0_vtbl;
    impl->IGameInput_v2_iface.lpVtbl = &game_input_v2_vtbl;
    impl->refcount = 1;
    list_init( &impl->devices );
    list_init( &impl->callbacks );
    list_init( &impl->callbacks_v2 );
    virtual_devices_init();

    if (!(impl->initialized = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        ERR( "Failed to create init event\n" );
        free( impl );
        return NULL;
    }

    /* Device enumeration goes through DevQuery, which drives a PnP-service RPC
     * that is not reliably available in the prefixes we target and can fault
     * its notification thread. The caller decides whether to run it; without
     * it the object is still valid, just with no enumerated devices. */
    if (devquery &&
        FAILED(DevCreateObjectQuery( DevObjectTypeDeviceInterface, DevQueryFlagUpdateResults | DevQueryFlagAllProperties,
                                     0, NULL, 0, NULL, device_query_cb, impl, &impl->query )))
    {
        WARN( "device enumeration unavailable; continuing with no devices\n" );
        impl->query = NULL;
    }
    if (!impl->query) SetEvent( impl->initialized );

    TRACE( "created GameInput %p\n", impl );
    return impl;
}

HRESULT WINAPI GameInputCreate( IGameInput_v0 **out )
{
    struct game_input *impl;
    BOOL opt_in, devquery;
    const char *env;

    /* The implementation below is complete enough to drive a title, but
     * upstream keeps it behind an allow-list (only Kingdom Come Deliverance II,
     * and never on Steam Deck) because device enumeration has not been broadly
     * validated. GDK/UWP titles such as Minecraft treat a missing GameInput
     * runtime as a fatal error ("GameInput Runtime could not be loaded"), so
     * WINE_GAMEINPUT=1 opts a launcher into the real object regardless. */
    if (!(opt_in = (env = getenv( "WINE_GAMEINPUT" )) && atoi( env )))
    {
        const char *sgi, *sd;
        if ((!(sgi = getenv( "SteamGameId" )) || strcmp(sgi, "1771300") /* Kingdom Come Deliverance II */)) goto failed;
        if ((sd = getenv( "SteamDeck" )) && atoi( sd )) goto failed; /* not on Steam Deck */
    }

    /* DevQuery stays off on the opt-in path by default (see game_input_create)
     * and WINE_GAMEINPUT_DEVQUERY turns it back on there. The allow-listed
     * title keeps it, as upstream always ran it: its IGameInput_v0 finds pads
     * only through that enumeration. */
    devquery = !opt_in || getenv( "WINE_GAMEINPUT_DEVQUERY" );

    TRACE( "out %p\n", out );

    EnterCriticalSection( &game_input_cs );
    if ((impl = game_input)) impl->refcount++;
    else impl = game_input = game_input_create( devquery );
    LeaveCriticalSection( &game_input_cs );
    if (!impl) return E_OUTOFMEMORY;

    /* Bounded wait: proceed with whatever devices enumerated rather than
     * hanging a title's startup forever if the query never settles. */
    WaitForSingleObject( impl->initialized, 5000 );
    *out = &impl->IGameInput_v0_iface;
    return S_OK;

failed:
    FIXME( "out %p, stub!\n", out );
    return E_NOTIMPL;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}
