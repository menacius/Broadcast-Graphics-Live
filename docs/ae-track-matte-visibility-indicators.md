# AE Track Matte Visibility and Indicators

This update makes layer-mask / track-matte behavior closer to After Effects:

- A layer selected as another layer's mask is no longer composited as normal visible artwork.
- The same layer is still rendered internally into the matte surface, so it continues to affect the masked layer.
- The masked layer still uses its selected Alpha / Inverted Alpha mask mode during rendering.
- The layer list header now includes a Matte indicator area.
- Layers that are used as masks and layers that are masked by another layer display timeline mask indicators in the layer row.

This keeps the mask layer functionally active while preventing it from appearing as a separate visible layer in the final title output.
