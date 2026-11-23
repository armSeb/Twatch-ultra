# Options de compilation MeshCore, communes au composant et aux fichiers de
# main qui incluent ses en-têtes (elles changent la taille des structures).
set(MESHCORE_DEFS
    ESP32=1
    USE_SX1262=1
    MAX_CONTACTS=350
    MAX_GROUP_CHANNELS=40
    OFFLINE_QUEUE_SIZE=256
    LORA_FREQ=869.618
    LORA_BW=62.5
    LORA_SF=8
    LORA_CR=8
    LORA_TX_POWER=22
    MAX_LORA_TX_POWER=22
)
set(MESHCORE_INC
    ${CMAKE_CURRENT_LIST_DIR}/shim
    ${CMAKE_CURRENT_LIST_DIR}/upstream/src
    ${CMAKE_CURRENT_LIST_DIR}/upstream/companion
    ${CMAKE_CURRENT_LIST_DIR}/upstream/lib/ed25519
)
