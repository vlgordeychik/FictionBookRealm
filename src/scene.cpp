#include "scene.h"
#include "scene_manager.h"

void Scene::go_back() {
    if (mgr_) mgr_->go_back();
}
