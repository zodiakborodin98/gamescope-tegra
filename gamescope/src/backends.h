#pragma once

namespace gamescope
{
    // Backend enum.
    enum GamescopeBackend
    {
        Auto,
        DRM,
        SDL,
        OpenVR,
        Headless,
        Wayland,
        TegraL4T,
    };

    // Backend forward declarations.
    class CSDLBackend;
    class CDRMBackend;
    class COpenVRBackend;
    class CHeadlessBackend;
    class CWaylandBackend;
    class CTegraL4TBackend;
}
