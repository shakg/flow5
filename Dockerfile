# flow5-mcp in a container: builds XFoil-lib, flow5-lib, flow5-io-lib and the
# MCP server, without the GUI app.
#
#   docker build -t flow5-mcp .
#   docker run -i --rm flow5-mcp            # speaks MCP over stdin/stdout
#
# Debian trixie is used because flow5-io-lib needs OpenCASCADE >= 7.8
# (TKDESTEP, TKDEOBJ, TKDESTL).

FROM debian:trixie AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make \
        qt6-base-dev qmake6 \
        libopenblas-pthread-dev liblapacke-dev \
        libocct-foundation-dev libocct-modeling-data-dev \
        libocct-modeling-algorithms-dev libocct-data-exchange-dev libocct-ocaf-dev \
        libgmsh-dev \
    && rm -rf /var/lib/apt/lists/*

# flow5 expects Fedora's OpenBLAS layout: <openblas/cblas.h>, <openblas/lapack.h> and -lopenblasp
RUN mkdir -p /usr/include/openblas \
    && ln -s /usr/include/x86_64-linux-gnu/openblas-pthread/*.h /usr/include/openblas/ \
    && ln -s /usr/include/lapack.h /usr/include/openblas/lapack.h \
    && ln -s /usr/lib/x86_64-linux-gnu/openblas-pthread/libopenblas.so /usr/lib/x86_64-linux-gnu/libopenblasp.so

WORKDIR /src
COPY XFoil-lib XFoil-lib
COPY flow5-lib flow5-lib
COPY flow5-io-lib flow5-io-lib
COPY flow5-mcp flow5-mcp

# -Wsfinae-incomplete is a GCC 16 option; trixie ships GCC 14
RUN for d in XFoil-lib flow5-lib flow5-io-lib flow5-mcp; do \
        (cd $d && qmake6 -after "QMAKE_CXXFLAGS -= -Wsfinae-incomplete=0" && make -j"$(nproc)") || exit 1; \
    done

RUN mkdir -p /out/lib /out/bin \
    && cp -P XFoil-lib/libXFoil.so* flow5-lib/libflow5-lib.so* flow5-io-lib/libflow5-io-lib.so* /out/lib/ \
    && cp flow5-mcp/flow5-mcp /out/bin/


FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        libqt6core6t64 libopenblas0-pthread \
        libocct-foundation-7.8 libocct-modeling-data-7.8 \
        libocct-modeling-algorithms-7.8 libocct-data-exchange-7.8 libocct-ocaf-7.8 \
        libgmsh4.13 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/lib/x86_64-linux-gnu/openblas-pthread/libopenblas.so.0 /usr/lib/x86_64-linux-gnu/libopenblasp.so.0

COPY --from=build /out/lib/ /usr/local/lib/
COPY --from=build /out/bin/flow5-mcp /usr/local/bin/
RUN ldconfig

# projects saved with project_save land here; mount a host directory to keep them
WORKDIR /data

ENTRYPOINT ["/usr/local/bin/flow5-mcp"]
