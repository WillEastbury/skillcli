# Inventory source onboarding

External inventories are discovery sources, not approved skills.

For each source:

1. Record its owner, repository, licence, retrieval date, taxonomy, scoring system, and
   adoption metric definitions.
2. Create a versioned mapping under `mappings/`.
3. Run `tools/normalize_inventory.py` to produce review candidates.
4. Preserve original taxonomy, scores, and adoption values in the review candidate.
5. Deduplicate candidates using source identity, aliases, and content fingerprints.
6. Review provenance, licence, skill files, requirements, and use cases.
7. Add an approved skill folder and canonical `skills.json` entry through a pull request.

Never treat unlike source scores as directly comparable. Normalised values must retain
their original scale and evidence, and may remain `null` when the source provides no
defensible measurement.
