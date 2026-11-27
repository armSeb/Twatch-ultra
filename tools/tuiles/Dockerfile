# Image de dessin des tuiles : MapLibre Native, sharp et la police Noto Sans
# viennent de l'image officielle de TileServer GL.
FROM maptiler/tileserver-gl:v5.7.0
COPY render.cjs style-montre.json entrypoint.sh /opt/twatch/
ENTRYPOINT ["/bin/bash", "/opt/twatch/entrypoint.sh"]
