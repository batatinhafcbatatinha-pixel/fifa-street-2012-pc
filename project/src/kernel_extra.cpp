// FIFA Street 4: absent USB camera support. The game can continue without it.
#include <rex/hook.h>

REX_STUB_RETURN(__imp__XUsbcamGetState, 0x48F)
REX_STUB_RETURN(__imp__XUsbcamSetConfig, 0x48F)
