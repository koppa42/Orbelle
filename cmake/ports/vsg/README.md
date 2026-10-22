# VSG Windows shared-library export overlay

Based on the fixed registry's VSG 1.1.15 port, with its unchanged source archive
and SHA512. Upstream `Presentation` omits `VSG_DECLSPEC`, so calling its public
`present()` from a Windows DLL consumer fails to link.

Port 1.1.15#1 adds the normal export decoration to that class. This lets
Orbelle inspect the actual `VkResult` from presentation. It retains the
upstream presentation implementation, release version and ABI version.
