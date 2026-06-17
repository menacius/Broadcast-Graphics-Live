# Auto Text Styling stop matching refinements

- Added strict stop matching per auto-style rule.
- When enabled, a rule is skipped if its configured `To` boundary is not found after the `From` boundary.
- This prevents live text cue edits from accidentally styling to the end of the line/textbox when a delimiter is missing.
- Added custom-marker inclusion controls:
  - Include the `From` custom character in the styled range.
  - Include the `To` custom character in the styled range.
- Added character-count and word-count marker options for rule boundaries.
- Persisted the new options in title JSON and included them in the cache hash.
