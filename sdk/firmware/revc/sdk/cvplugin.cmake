# cvplugin.cmake -- building a plugin for the SD cart's menu (docs/plugins.md).
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../sdk/cvplugin.cmake)    # after pico_sdk_init()
#   add_executable(nesinfo nesinfo.c ...)
#   cvkernel_use(nesinfo)        the console side: PSG Synth's kernel, its
#                                screen and the RESET handoff (cvkernel.h,
#                                cvscreen.h); main() includes rom_image.h
#                                and handoff.h for cvk_start()
#   cvsd_use(nesinfo)            the card (cvsd.h, ../sdplayer/fsro.h)
#   cvframes_use(nes)            the console side instead of cvkernel_use:
#                                the Game Boy kernel's frame lists
#                                (cvframes.h); main() includes gbkernel.h
#                                and handoff.h for cvf_start(); and
#                                cvdiag.h, where it got to in the trace
#   cvframes_serve2(nes)         after cvframes_use, for every new frame
#                                plugin: core 1's bus loop is cvfserve.S,
#                                which acts on two samples of the pins that
#                                agree (the old loop is still the default)
#   cvpsram_use(nes)             the PSRAM (cvpsram.h)
#   (before pico_sdk_import.cmake: include(.../sdk/cvpad_tinyusb.cmake), for
#   cvpad_use's USB host from sdk/tinyusb, TinyUSB 0.21.0, whose tuh_task()
#   doesn't block while a device enumerates; without it, the pico-sdk's 0.18)
#   cvpad_use(nes [OWN_TUSB_CONFIG])  USB game pads (cvpad.h): the USB
#                                port becomes a host; TinyUSB's configuration
#                                is cvpad_tusb/tusb_config.h (first on the
#                                path: midihello's directory has another),
#                                or with OWN_TUSB_CONFIG the plugin's own
#                                (host, HID); USES usb-host
#   cvcart_features(myplugin [PADS] [KEYBOARD] [MOUSE] [MIDI_IN] [MIDI_OUT]
#                   [PSRAM] [CARD])
#                                what the plugin uses, each a switch, before
#                                cvcart_plugin(): the USB ones are cvusb.h
#                                (instead of cvpad_use), PSRAM cvpsram_use,
#                                CARD cvsd_use; and the .CVP's USES follow
#                                (usb-host, usb-device, psram)
#   cvfont_use(myplugin)         the 6x8 font with lower case (cvfont.h): 40
#                                columns on a canvas (../midihello/vdp.h)
#   cvedit_use(myplugin)         the text editor (cvedit.h), with cvfont_use;
#                                the plugin also needs cvkernel_use (the
#                                canvas) and the KEYBOARD and CARD features
#   cvcart_plugin(nesinfo ID parrotcart.nesinfo NAME "NES Info" VERSION 1.0.0
#                 [BY "Parrot Cart"] [HOME /NES] [OPENS .nes ...] [USES usb-host standalone ...]
#                 [ICON icon.txt] [FILE_ICON icon.txt] [FILE NESINFO.CVP] [HEAP]
#                 [README README.md])
#
# USES: what the plugin uses (usb-host, usb-device, psram; cvcart_features
# adds these), and standalone: the plugin opens files (OPENS) but can also
# start with none, an empty page or a demo of its own. The menu lists it
# with the plugins that open no files, which always start on their own.
# Without it, a plugin with OPENS starts only with one of its files.
#
# README: the plugin's README, which its details page on the cart shows a
# page at a time. Write ordinary Markdown: cvreadme.py turns it into the
# cart's text (30 columns, ASCII) when the plugin is built, and
# `python3 cvreadme.py README.md` prints what the cart will show. Credits,
# licences and where the source is belong there.
#
# HEAP: the plugin may use malloc and new (an emulator core does): the heap
# stops below the launch block (cvheap.h). Without it, linking malloc fails
# the build.
#
# cvcart_plugin makes it a no_flash binary packaged with a relative load map
# (the bootrom copies it from the menu's flash slot into SRAM), then the
# plugin file: build/NESINFO.CVP (the target's name in capitals, or FILE),
# which goes in the card's /PLUGINS folder.
#
# XIP: a plugin too big for SRAM (the SCUMM engine) stays an ordinary flash
# binary, linked at 0x10000000, and runs in place from its slot: the bootrom
# maps the slot at 0x10000000 when the menu starts it. At most 2 MB less 4K:
# one over 1 MB less 4K takes two of the cache's 1 MB slots side by side.
# Its core 1 code must still run from SRAM (__scratch_x, __not_in_flash_func:
# cvframes.c's does). HEAP isn't for it: it brings its own _sbrk. Its main()
# calls cvxip_fast() first (cvxip.h): the slot starts in slow serial reads.
#
# Needs sjasmplus (the kernel), python3 (the pin order, from hw3/pins.py;
# mkcvp.py) and picotool.
set(CVSDK_DIR ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(CVSDK_REVC ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

# the kernel's image and the handoff, once a build directory
function(cvkernel_generate)
    get_property(done DIRECTORY PROPERTY CVKERNEL_GEN)
    if (done)
        return()
    endif()
    set_property(DIRECTORY PROPERTY CVKERNEL_GEN TRUE)
    set(MH ${CVSDK_REVC}/midihello)
    set(GEN ${CMAKE_CURRENT_BINARY_DIR}/cvkernel)
    file(MAKE_DIRECTORY ${GEN})
    add_custom_command(
        OUTPUT ${GEN}/mhkernel.bin ${GEN}/mhkernel.sym
        COMMAND sjasmplus --nologo --sym=${GEN}/mhkernel.sym ${MH}/midihello.asm
        WORKING_DIRECTORY ${GEN}            # SAVEBIN writes to the cwd
        DEPENDS ${MH}/midihello.asm)
    add_custom_command(
        OUTPUT ${GEN}/rom_image.h
        COMMAND python3 ${CVSDK_REVC}/bin2h_revc.py ${GEN}/mhkernel.bin ${GEN}/rom_image.h
        DEPENDS ${GEN}/mhkernel.bin ${CVSDK_REVC}/bin2h_revc.py ${CVSDK_REVC}/../../hw3/pins.py)
    add_custom_command(
        OUTPUT ${GEN}/handoff.bin
        COMMAND sjasmplus --nologo ${MH}/handoff.asm
        WORKING_DIRECTORY ${GEN}
        DEPENDS ${MH}/handoff.asm)
    add_custom_command(
        OUTPUT ${GEN}/handoff.h
        COMMAND python3 ${CVSDK_REVC}/sdplayer/bin2c.py ${GEN}/handoff.bin handoff_bin ${GEN}/handoff.h
        DEPENDS ${GEN}/handoff.bin ${CVSDK_REVC}/sdplayer/bin2c.py)
endfunction()

# the Game Boy kernel, the handoff and the pin order, once a build directory
function(cvframes_generate)
    get_property(done DIRECTORY PROPERTY CVFRAMES_GEN)
    if (done)
        return()
    endif()
    set_property(DIRECTORY PROPERTY CVFRAMES_GEN TRUE)
    set(GEN ${CMAKE_CURRENT_BINARY_DIR}/cvframes)
    set(GBK ${CVSDK_REVC}/../../gbcv/kernel/gbkernel.asm)
    set(HO ${CVSDK_REVC}/midihello/handoff.asm)
    file(MAKE_DIRECTORY ${GEN})
    add_custom_command(
        OUTPUT ${GEN}/gbkernel.bin
        COMMAND sjasmplus --nologo ${GBK}
        WORKING_DIRECTORY ${GEN}            # SAVEBIN writes to the cwd
        DEPENDS ${GBK})
    add_custom_command(
        OUTPUT ${GEN}/gbkernel.h
        COMMAND python3 ${CVSDK_REVC}/sdplayer/bin2c.py ${GEN}/gbkernel.bin gbkernel_bin ${GEN}/gbkernel.h
        DEPENDS ${GEN}/gbkernel.bin ${CVSDK_REVC}/sdplayer/bin2c.py)
    add_custom_command(
        OUTPUT ${GEN}/handoff.bin
        COMMAND sjasmplus --nologo ${HO}
        WORKING_DIRECTORY ${GEN}
        DEPENDS ${HO})
    add_custom_command(
        OUTPUT ${GEN}/handoff.h
        COMMAND python3 ${CVSDK_REVC}/sdplayer/bin2c.py ${GEN}/handoff.bin handoff_bin ${GEN}/handoff.h
        DEPENDS ${GEN}/handoff.bin ${CVSDK_REVC}/sdplayer/bin2c.py)
    add_custom_command(
        OUTPUT ${GEN}/pins.h
        COMMAND python3 ${CVSDK_DIR}/mkpins.py ${GEN}/pins.h
        DEPENDS ${CVSDK_DIR}/mkpins.py ${CVSDK_REVC}/../../hw3/pins.py)
endfunction()

function(cvframes_use TARGET)
    cvframes_generate()
    set(GEN ${CMAKE_CURRENT_BINARY_DIR}/cvframes)
    target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvframes.c ${CVSDK_DIR}/cvdiag.c ${GEN}/gbkernel.h ${GEN}/handoff.h ${GEN}/pins.h)
    target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR} ${GEN})
    target_link_libraries(${TARGET} pico_stdlib pico_multicore hardware_watchdog hardware_exception)
    # panic(): its message and caller in the menu's note (cvdiag.c)
    target_compile_definitions(${TARGET} PRIVATE PICO_PANIC_FUNCTION=cvd_panic)
endfunction()

# cvframes_serve2(nes), after cvframes_use: core 1's bus loop is cvfserve.S,
# the games' loop (cvplay/cvserve.S, CVSERVE.md) for the frame lists: nothing
# is made of one sample of the pins, and a select-like address no longer
# stops the serving. Without it the loop is cvframes.c's own, as before.
function(cvframes_serve2 TARGET)
    target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvfserve.S)
    target_compile_definitions(${TARGET} PRIVATE CVF_SERVE2)
endfunction()

function(cvpsram_use TARGET)
    target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvpsram.c)
    target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
    target_link_libraries(${TARGET} pico_stdlib hardware_clocks hardware_xip_cache)
endfunction()

function(cvpad_use TARGET)
    cmake_parse_arguments(P "OWN_TUSB_CONFIG" "" "" ${ARGN})
    get_target_property(features ${TARGET} CVCART_FEATURES)
    if (features MATCHES "PADS|KEYBOARD|MOUSE|MIDI")
        message(FATAL_ERROR "cvpad_use(${TARGET}) with cvcart_features()'s USB features: PADS there takes its place")
    endif()
    target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvpad.c ${CVSDK_DIR}/swpro.c ${CVSDK_DIR}/hidpad.c)
    if (NOT CVPAD_TINYUSB_VENDORED)
        # the pico-sdk's TinyUSB 0.18 (no cvpad_tinyusb.cmake): its hub
        # driver asks for the hub descriptor with wValue 0, which a VIA
        # VL817 hub stalls: midihello's fixed copy instead (sdk/tinyusb's
        # hub.c has the fix)
        target_sources(${TARGET} PRIVATE ${CVSDK_REVC}/midihello/tinyusb_fix/hub.c)
        set_source_files_properties(${PICO_TINYUSB_PATH}/src/host/hub.c
            TARGET_DIRECTORY ${TARGET} PROPERTIES HEADER_FILE_ONLY TRUE)
    endif()
    target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
    if (NOT P_OWN_TUSB_CONFIG)
        target_include_directories(${TARGET} BEFORE PRIVATE ${CVSDK_DIR}/cvpad_tusb)
    endif()
    target_link_libraries(${TARGET} tinyusb_host hardware_timer)
endfunction()

# cvcart_features(TARGET feature ...): the switches (cvusb.h has the USB ones).
#
#   PADS KEYBOARD MOUSE MIDI_IN    the USB port a host (usb-host)
#   MIDI_OUT                       the USB port a MIDI device on a computer (usb-device)
#   PSRAM                          the 8 MB PSRAM: cvpsram_use (psram)
#   CARD                           the SD card: cvsd_use
#
# For the USB features: cvusb.c and only the parts they need, TinyUSB from
# sdk/tinyusb (cvpad_tinyusb.cmake, included before pico_sdk_import.cmake)
# with cvusb_tusb/tusb_config.h, whose classes follow the CVUSB_* definitions
# set here. Call it before cvcart_plugin(), which adds the USES it sets to
# its own. It takes the place of cvpad_use() (the two can't be together).
function(cvcart_features TARGET)
    set(known PADS KEYBOARD MOUSE MIDI_IN MIDI_OUT PSRAM CARD)
    cmake_parse_arguments(F "${known}" "" "" ${ARGN})
    if (F_UNPARSED_ARGUMENTS)
        string(REPLACE ";" " " known_s "${known}")
        message(FATAL_ERROR "cvcart_features(${TARGET}): ${F_UNPARSED_ARGUMENTS}? The features are ${known_s}")
    endif()
    get_target_property(done ${TARGET} CVCART_PLUGIN)
    if (done)
        message(FATAL_ERROR "cvcart_features(${TARGET}) after cvcart_plugin(${TARGET}): it sets the plugin's USES, so it comes first")
    endif()
    get_target_property(had ${TARGET} CVCART_FEATURES)
    if (had)
        message(FATAL_ERROR "cvcart_features(${TARGET}) twice: name every feature in one call")
    endif()
    set(features)
    set(uses)
    set(usb_src)
    foreach (f ${known})
        if (F_${f})
            list(APPEND features ${f})
        endif()
    endforeach()
    set_property(TARGET ${TARGET} PROPERTY CVCART_FEATURES "${features};")
    if (F_PADS OR F_KEYBOARD OR F_MOUSE OR F_MIDI_IN)
        list(APPEND uses usb-host)
    endif()
    if (F_MIDI_OUT)
        list(APPEND uses usb-device)
    endif()
    if (F_PSRAM)
        list(APPEND uses psram)
        cvpsram_use(${TARGET})
    endif()
    if (F_CARD)
        cvsd_use(${TARGET})
    endif()
    if (F_PADS OR F_KEYBOARD OR F_MOUSE OR F_MIDI_IN OR F_MIDI_OUT)
        if (NOT CVPAD_TINYUSB_VENDORED)
            message(FATAL_ERROR "cvcart_features(${TARGET}): the USB features need the SDK's TinyUSB: "
                "include(.../sdk/cvpad_tinyusb.cmake) before pico_sdk_import.cmake")
        endif()
        get_target_property(src ${TARGET} SOURCES)
        if (src MATCHES "cvpad\\.c")
            message(FATAL_ERROR "cvcart_features(${TARGET}): with cvpad_use() too; PADS takes its place")
        endif()
        list(APPEND usb_src ${CVSDK_DIR}/cvusb.c)
        foreach (f PADS KEYBOARD MOUSE MIDI_IN MIDI_OUT)
            if (F_${f})
                target_compile_definitions(${TARGET} PRIVATE CVUSB_${f}=1)
            endif()
        endforeach()
        if (F_PADS)
            list(APPEND usb_src ${CVSDK_DIR}/cvpad.c ${CVSDK_DIR}/swpro.c ${CVSDK_DIR}/hidpad.c)
            target_compile_definitions(${TARGET} PRIVATE CVUSB=1)    # cvpad.c's hooks for cvusb.c
        endif()
        if (F_KEYBOARD OR F_MOUSE)
            list(APPEND usb_src ${CVSDK_DIR}/cvusb_keys.c)
        endif()
        if (F_MIDI_IN)
            list(APPEND usb_src ${CVSDK_DIR}/cvusb_midih.c)
        endif()
        if (F_MIDI_OUT)
            list(APPEND usb_src ${CVSDK_DIR}/cvusb_midid.c)
        endif()
        if (F_MIDI_IN OR F_MIDI_OUT)
            list(APPEND usb_src ${CVSDK_DIR}/cvusb_midi.c)
        endif()
        target_sources(${TARGET} PRIVATE ${usb_src})
        target_include_directories(${TARGET} BEFORE PRIVATE ${CVSDK_DIR}/cvusb_tusb)
        target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
        target_link_libraries(${TARGET} pico_stdlib hardware_timer)
        if (F_PADS OR F_KEYBOARD OR F_MOUSE OR F_MIDI_IN)
            target_link_libraries(${TARGET} tinyusb_host)
        endif()
        if (F_MIDI_OUT)
            target_link_libraries(${TARGET} tinyusb_device)
        endif()
    endif()
    set_property(TARGET ${TARGET} PROPERTY CVCART_USES "${uses}")
endfunction()

function(cvfont_use TARGET)
    get_target_property(src ${TARGET} SOURCES)
    if (NOT src MATCHES "cvfont\\.c")
        target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvfont.c)
    endif()
    target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
endfunction()

# the editor: its files come through fsro (cvsd_use, or cvcart_features CARD)
function(cvedit_use TARGET)
    cvfont_use(${TARGET})
    target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvedit.c)
    target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
endfunction()

function(cvkernel_use TARGET)
    cvkernel_generate()
    set(GEN ${CMAKE_CURRENT_BINARY_DIR}/cvkernel)
    target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvkernel.c ${CVSDK_DIR}/cvscreen.c
                   ${CVSDK_REVC}/midihello/vdp.c ${GEN}/rom_image.h ${GEN}/handoff.h)
    target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR} ${GEN})
    target_link_libraries(${TARGET} pico_stdlib pico_multicore hardware_watchdog)
endfunction()

function(cvsd_use TARGET)
    target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvsd.c ${CVSDK_REVC}/sdplayer/fsro.c)
    target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
    target_link_libraries(${TARGET} pico_stdlib hardware_spi hardware_watchdog)
endfunction()

function(cvcart_plugin TARGET)
    cmake_parse_arguments(P "HEAP;XIP" "ID;NAME;VERSION;BY;HOME;ICON;FILE_ICON;FILE;README" "OPENS;USES" ${ARGN})
    if (NOT P_ID OR NOT P_NAME OR NOT P_VERSION)
        message(FATAL_ERROR "cvcart_plugin(${TARGET}): ID, NAME and VERSION are needed")
    endif()
    if (NOT P_FILE)
        string(TOUPPER ${TARGET}.CVP P_FILE)
    endif()
    if (NOT P_XIP)
        pico_set_binary_type(${TARGET} no_flash)
        pico_package_uf2_output(${TARGET} 0x10E00000)   # any flash address: the load map is relative
    endif()
    # pico_add_extra_outputs' steps but the .hex: this toolchain's objcopy
    # (binutils 2.33) can crash writing Intel HEX for a no_flash image
    pico_add_dis_output(${TARGET})
    if (NOT PICO_NO_PICOTOOL)
        picotool_postprocess_binary(${TARGET} IS_ENCRYPTED)
    endif()
    pico_add_bin_output(${TARGET})
    pico_add_map_output(${TARGET})
    pico_add_uf2_output(${TARGET})
    # its RAM stops below the launch block (reserve.ld, cvlaunch.h), and no heap grows into it
    target_link_options(${TARGET} PRIVATE ${CVSDK_DIR}/reserve.ld)
    set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS ${CVSDK_DIR}/reserve.ld)
    if (P_XIP)
        # its own heap (an _sbrk of its own); cvxip_fast() for quad flash reads (cvxip.h)
        target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvxip.c)
        target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
    elseif (P_HEAP)
        target_sources(${TARGET} PRIVATE ${CVSDK_DIR}/cvheap.c)   # malloc, with the heap capped
        target_include_directories(${TARGET} PRIVATE ${CVSDK_DIR})
    else()
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND sh -c "if ${CMAKE_NM} $<TARGET_FILE:${TARGET}> | grep -qE ' (malloc|_malloc_r|__wrap_malloc)$'; then echo '${TARGET} links malloc: its heap would grow into the launch block (sdk/reserve.ld; or cvcart_plugin HEAP)'; rm -f $<TARGET_FILE:${TARGET}>; exit 1; fi"
            VERBATIM)
    endif()
    set(args --id ${P_ID} --name ${P_NAME} --version ${P_VERSION})
    set(deps ${CVSDK_REVC}/sdplayer/mkcvp.py)
    if (P_BY)
        list(APPEND args --by ${P_BY})
    endif()
    if (P_HOME)
        list(APPEND args --home ${P_HOME})
    endif()
    if (P_OPENS)
        list(APPEND args --opens ${P_OPENS})
    endif()
    # what cvcart_features() said, and what's given here
    get_target_property(f_uses ${TARGET} CVCART_USES)
    if (f_uses)
        list(APPEND P_USES ${f_uses})
        list(REMOVE_DUPLICATES P_USES)
    endif()
    if (P_USES)
        string(REPLACE ";" "," uses "${P_USES}")
        list(APPEND args --uses ${uses})
    endif()
    # a MIDI device's name on the computer: the plugin's (cvusb_midid.c)
    get_target_property(features ${TARGET} CVCART_FEATURES)
    if (features MATCHES "MIDI_OUT")
        target_compile_definitions(${TARGET} PRIVATE "CVUSB_DEVICE_NAME=\"${P_NAME}\"")
    endif()
    set_property(TARGET ${TARGET} PROPERTY CVCART_PLUGIN TRUE)
    if (P_ICON)
        get_filename_component(icon ${P_ICON} ABSOLUTE)
        list(APPEND args --icon ${icon})
    endif()
    if (P_FILE_ICON)
        get_filename_component(file_icon ${P_FILE_ICON} ABSOLUTE)
        list(APPEND args --file-icon ${file_icon})
    endif()
    if (P_XIP)
        list(APPEND args --xip)
    endif()
    if (P_README)
        get_filename_component(readme ${P_README} ABSOLUTE)
        list(APPEND args --readme ${readme})
        # (POST_BUILD steps run only when the target links: a README edited
        # alone is picked up by touching the target's link)
        set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS ${readme} ${CVSDK_DIR}/cvreadme.py)
    endif()
    # after the .uf2: POST_BUILD steps run in order
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND python3 ${CVSDK_REVC}/sdplayer/mkcvp.py ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.uf2
                ${CMAKE_CURRENT_BINARY_DIR}/${P_FILE} ${args}
        COMMENT "Plugin file ${P_FILE}"
        VERBATIM)
endfunction()
