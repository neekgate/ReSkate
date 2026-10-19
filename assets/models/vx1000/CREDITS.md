# The Filmer's camera model

Licensed CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/), which allows it to be shipped,
changed and shared as long as the author is credited and changes are noted.

| File | Title | Author | Source |
| --- | --- | --- | --- |
| `camcorder_video_camera.glb` | Camcorder Video Camera | James.Hasegawa (https://sketchfab.com/James.Hasegawa) | https://sketchfab.com/3d-models/camcorder-video-camera-002d346e9faa41b6b8075feced9e0ba7 |

`vx1000.glb` is the game model, made from `camcorder_video_camera.glb` by `prepare_camcorder.py`:
the outline copy left out, the parts merged by material, scaled to 0.40 m long, turned to look down
-Z with Y up, the origin moved to the middle of the top handle (where the hand grips it), and the
near-black plastics lifted to dark grey:

    python3 assets/models/vx1000/prepare_camcorder.py assets/models/vx1000/camcorder_video_camera.glb assets/models/vx1000/vx1000.glb

Build it with ReSkate Studio as `reskate/filmer/vx1000`.

In game the credit reads: "Camera model: Camcorder Video Camera by James.Hasegawa (CC BY 4.0), rescaled for the game."
