set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
# -Os: this app has to fit a small factory partition, including on the 4MB V1.
rg_setup_compile_options(-Os)
