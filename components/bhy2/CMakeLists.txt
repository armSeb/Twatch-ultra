# BHY2 SensorAPI de Bosch Sensortec (BSD-3-Clause), v1.6.0, sans modification,
# et le firmware standard du BHI260AP (BHI260AP.fw, chargé en RAM au démarrage).
idf_component_register(
    SRCS "src/bhy2.c" "src/bhy2_hif.c" "src/bhy2_parse.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "firmware/BHI260AP.fw"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable)
