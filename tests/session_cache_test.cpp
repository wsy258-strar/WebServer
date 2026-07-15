#include "cache/SessionCache.h"

#include <cassert>

int main()
{
    Session input(9, "token-9", 7, "scene-2", 1, "created", "updated");
    Session output;

    assert(SessionCache::deserialize(SessionCache::serialize(input), output));
    assert(output.id == 9);
    assert(output.sessionToken == "token-9");
    assert(output.userId == 7);
    assert(output.sceneId == "scene-2");
    assert(output.status == 1);
    assert(output.createdAt == "created");
    assert(output.updatedAt == "updated");
    assert(!SessionCache::deserialize("bad", output));
}
