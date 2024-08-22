def getRoot(config):
    if not config.parent:
        return config
    return getRoot(config.parent)


root = getRoot(config)

if "libdispatch" in root.available_features:
    additional_cflags = " -fblocks "
    for index, (template, replacement) in enumerate(config.substitutions):
        if template in ["%clang_psan ", "%clangxx_psan "]:
            config.substitutions[index] = (template, replacement + additional_cflags)
else:
    config.unsupported = True

if config.host_os == "Darwin":
    config.environment["PSAN_OPTIONS"] += ":ignore_noninstrumented_modules=1"
