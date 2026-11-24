file(GLOB app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
file(GLOB font_sources ${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.c)

# MeshCore : exemple companion_radio d'origine + couche matérielle de la montre
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/meshcore/meshcore_defs.cmake)
set(mc_up ${CMAKE_CURRENT_SOURCE_DIR}/../components/meshcore/upstream)
file(GLOB mc_port ${CMAKE_CURRENT_SOURCE_DIR}/meshcore/*.cpp)
file(GLOB mc_c ${CMAKE_CURRENT_SOURCE_DIR}/meshcore/*.c)
set(mc_sources ${mc_port} ${mc_up}/companion/MyMesh.cpp ${mc_up}/companion/DataStore.cpp
    ${mc_up}/src/helpers/CommonRadioPrefs.cpp)

idf_component_register(
    SRCS ${app_sources} ${font_sources} ${mc_sources} ${mc_c}
    INCLUDE_DIRS "." "meshcore"
)

# Avertissements stricts sur notre code (pas sur les polices générées)
set_source_files_properties(${app_sources}
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-missing-field-initializers"
)
set_source_files_properties(${mc_sources}
    PROPERTIES COMPILE_DEFINITIONS "${MESHCORE_DEFS}"
               COMPILE_OPTIONS "-Wno-reorder;-Wno-unused-variable;-Wno-sign-compare;-Wno-format;-Wno-missing-field-initializers;-Wno-unused-parameter"
)
# historique MeshCore (C) : nos avertissements stricts, tailles de MeshCore
set_source_files_properties(${mc_c}
    PROPERTIES COMPILE_DEFINITIONS "${MESHCORE_DEFS}"
               COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-missing-field-initializers"
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

# Pack d'émojis (tools/gen_emoji.py) écrit dans sa partition par « idf.py flash »
esptool_py_flash_to_partition(flash "emoji" "${CMAKE_CURRENT_SOURCE_DIR}/../assets/emoji.bin")
