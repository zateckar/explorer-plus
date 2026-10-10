#pragma once
// Property keys missing from this MinGW's propkey.h but stable in the Windows SDK.
#include <propkey.h>

// Declared here; DEFINED once in settings-independent TU (shell_utils.cpp) via
//      #define INITGUID + #include "pkeys_compat_def.h"

EXTERN_C const PROPERTYKEY PKEY_Size;
EXTERN_C const PROPERTYKEY PKEY_ItemTypeText;
EXTERN_C const PROPERTYKEY PKEY_Image_HorizontalSize;
EXTERN_C const PROPERTYKEY PKEY_Image_VerticalSize;
EXTERN_C const PROPERTYKEY PKEY_ItemNameDisplay;
EXTERN_C const PROPERTYKEY PKEY_ItemFolderPathDisplay;
EXTERN_C const PROPERTYKEY PKEY_Image_Dimensions;
