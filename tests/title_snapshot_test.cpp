#include "title-snapshot.h"

#include <cassert>
#include <iostream>
#include <memory>

int main()
{
    Title title;
    title.id = "snapshot-title";
    auto layer = std::make_shared<Layer>();
    layer->id = "layer";
    layer->name = "original";
    title.layers.push_back(layer);

    Title snapshot = clone_title_snapshot(title);
    assert(snapshot.id == title.id);
    assert(snapshot.layers.size() == 1);
    assert(snapshot.layers[0]);
    assert(snapshot.layers[0].get() != title.layers[0].get());

    snapshot.layers[0]->name = "snapshot";
    assert(title.layers[0]->name == "original");

    auto shared_title = std::make_shared<Title>(title);
    auto shared_snapshot = clone_title_snapshot(shared_title);
    assert(shared_snapshot);
    assert(shared_snapshot.get() != shared_title.get());
    assert(shared_snapshot->layers[0].get() != shared_title->layers[0].get());

    std::cout << "deep title snapshot contract passed\n";
    return 0;
}
