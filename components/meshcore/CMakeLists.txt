# MeshCore (github.com/meshcore-dev/MeshCore, MIT) : cœur du protocole, aides
# et ed25519 d'orlp, plus une fine couche Arduino → ESP-IDF (shim/).
# L'exemple companion_radio et la couche matérielle sont compilés dans main
# (main/meshcore/), qui fournit target.h.
include(${CMAKE_CURRENT_LIST_DIR}/meshcore_defs.cmake)

set(up upstream/src)
idf_component_register(
    SRCS
        ${up}/Dispatcher.cpp ${up}/Identity.cpp ${up}/Mesh.cpp ${up}/Packet.cpp ${up}/Utils.cpp
        ${up}/helpers/AdvertDataHelpers.cpp ${up}/helpers/BaseChatMesh.cpp
        ${up}/helpers/IdentityStore.cpp ${up}/helpers/StaticPoolPacketManager.cpp
        ${up}/helpers/TxtDataHelpers.cpp ${up}/helpers/TransportKeyStore.cpp
        ${up}/helpers/RegionMap.cpp ${up}/helpers/ConfigSerializer.cpp
        ${up}/helpers/DynamicConfigSerializer.cpp
        upstream/lib/ed25519/add_scalar.c upstream/lib/ed25519/fe.c upstream/lib/ed25519/ge.c
        upstream/lib/ed25519/key_exchange.c upstream/lib/ed25519/keypair.c
        upstream/lib/ed25519/sc.c upstream/lib/ed25519/seed.c upstream/lib/ed25519/sha512.c
        upstream/lib/ed25519/sign.c upstream/lib/ed25519/verify.c
        shim/arduino.cpp shim/crypto.cpp shim/fs.cpp
    INCLUDE_DIRS ${MESHCORE_INC}
    REQUIRES mbedtls fatfs vfs wear_levelling esp_timer
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${MESHCORE_DEFS})
# code d'origine : on ne le réécrit pas pour des avertissements
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-reorder -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-sign-compare -Wno-format -Wno-missing-field-initializers -Wno-unused-function)
