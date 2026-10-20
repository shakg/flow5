
#    Compilation instructions:
#    https://flow5.tech/docs/flow5_doc/Source/Compilation.html


TEMPLATE = subdirs


SUBDIRS = \
    XFoil-lib \
    flow5-lib \
    flow5-io-lib \
    flow5-app \
    flow5-mcp \

flow5-mcp.depends = XFoil-lib flow5-lib flow5-io-lib


TRANSLATIONS = meta/translations/flow5_zh_CN.ts \
               meta/translations/flow5_fr_FR.ts \

DISTFILES += \
                LICENSE \
                README.md