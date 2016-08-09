# set library parameters
SET (name p2p_route)

# use generic plugin maker
oonf_create_plugin("${name}" "${name}.c" "${name}.h" "")
