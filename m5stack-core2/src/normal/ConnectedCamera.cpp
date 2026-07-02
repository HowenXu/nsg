#include "ConnectedCamera.h"

ConnectedCamera::ConnectedCamera(const SavedCameraInfo& info) : info(info), pClient(nullptr) {}

ConnectedCamera::~ConnectedCamera() = default;
