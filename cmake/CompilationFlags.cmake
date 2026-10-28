# sgns-blockstore - compilation flags for the cmaketemplate build path.
#
# Deliberately minimal: sgns-blockstore is a dependency library consumed by
# SuperGenius and GeniusSDK; the template's CommonCompilerOptions defaults
# apply, and warning walls that make sense for applications only add noise
# (and risk) to a library consumed through installed headers. The repo's
# Tier-1 hygiene suites (header_hygiene / tier1_static_audit) carry the
# real source-quality gates.
