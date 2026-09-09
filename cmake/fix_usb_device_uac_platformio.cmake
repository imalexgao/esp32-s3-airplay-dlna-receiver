# espressif/usb_device_uac attaches its descriptor source to the TinyUSB
# component with target_sources(... PUBLIC ...), which lands the file in both
# SOURCES and INTERFACE_SOURCES. The INTERFACE half propagates to every
# dependent of TinyUSB, so PlatformIO/SCons compiled usb_descriptors.c twice
# with different flags.
#
# Drop only the INTERFACE copy. The file stays in TinyUSB's own SOURCES, so it
# is compiled exactly once and tud_descriptor_device_cb() and friends still
# resolve — stripping SOURCES as well is what used to make every
# CONFIG_AUDIO_OUTPUT_USB build fail to link.
idf_build_get_property(build_components BUILD_COMPONENTS)
set(tinyusb_component "")
if(espressif__tinyusb IN_LIST build_components)
    set(tinyusb_component espressif__tinyusb)
elseif(tinyusb IN_LIST build_components)
    set(tinyusb_component tinyusb)
endif()

if(tinyusb_component)
    idf_component_get_property(tusb_lib ${tinyusb_component} COMPONENT_LIB)
    get_target_property(tusb_sources ${tusb_lib} INTERFACE_SOURCES)
    if(tusb_sources)
        set(filtered_tusb_sources "")
        foreach(src IN LISTS tusb_sources)
            if(NOT src MATCHES "usb_device_uac[/\\\\]tusb[/\\\\]usb_descriptors\\.c$")
                list(APPEND filtered_tusb_sources "${src}")
            endif()
        endforeach()
        set_target_properties(${tusb_lib} PROPERTIES
            INTERFACE_SOURCES "${filtered_tusb_sources}")
    endif()
endif()
