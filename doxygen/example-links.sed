# Doxygen input filter for the Markdown pages (FILTER_PATTERNS in Doxyfile). The pages link the
# example sources by relative path, which works on GitHub; Doxygen's HTML needs a reference to the
# file's own page instead. Rewrites `](../examples/NAME)` and `](../../examples/NAME)` to `](@ref NAME)`.
s#\]\((\.\./)+examples/([A-Za-z0-9_.-]+)\)#](@ref \2)#g
