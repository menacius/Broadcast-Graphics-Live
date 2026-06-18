# Shape Tool: No Body Move for Existing Shapes

When the Shape tool is active, dragging the body/interior of an already existing
shape no longer starts a Move manipulation.  Body drag is kept for creating a new
shape, which matches the draw-tool interaction model.

Selected shape layers can still be manipulated through explicit canvas handles:
resize handles, rotation handles, origin handles, and corner radius handles still
work while the Shape tool is active.

Text layers keep their existing Text tool body-move behavior.
