#pragma once

// Forward re-light of nearby static geometry inside the flashlight cone.
//
// The screen space flashlight shades against TESR_NormalsBuffer, which is reconstructed
// by differencing depth. That is fine on large flat surfaces and poor on anything with
// detail, and it knows nothing about a mesh's own normal map, gloss or specular settings.
// This pass re-draws the geometry that is inside the cone with its real vertex normals,
// tangent basis, diffuse texture and normal map, and adds the result to the scene.
//
// Off by default. Adapted from Better Flashlight NVSE, which is a fork of this project.
namespace MaterialPass {
	void BeginFrame(bool abActive);			// resets the queue, called once per frame
	void CaptureScene(SceneGraph* apSceneGraph);
	void CaptureGeometry(NiGeometry* apGeometry);
	void RenderWorld();
}
