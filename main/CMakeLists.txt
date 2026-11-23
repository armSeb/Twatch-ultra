file(GLOB app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
file(GLOB font_sources ${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.c)

idf_component_register(
    SRCS ${app_sources} ${font_sources}
    INCLUDE_DIRS "."
)

# Avertissements stricts sur notre code (pas sur les polices générées)
set_source_files_properties(${app_sources}
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-missing-field-initializers"
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

# Pack d'émojis (tools/gen_emoji.py) écrit dans sa partition par « idf.py flash »
esptool_py_flash_to_partition(flash "emoji" "${CMAKE_CURRENT_SOURCE_DIR}/../assets/emoji.bin")
