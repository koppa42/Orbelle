# Historical project files

Keep these files in their original format. They verify that later writers do not become the only source of test inputs for backward compatibility.

| File | Origin and purpose | SHA-256 |
| --- | --- | --- |
| `project-v1.orbitforge` | Phase 1 Python save-project example: two-body and J2 objects, no display names or data binding | `ab0976fffc2713642ab6890753ef75e0d3c7f3cf6d00469866da1fdfdfae484d` |
| `project-v2.orbitforge` | Phase 2 desktop smoke output before LOCK was introduced: two objects, a Chinese display name and historical EOP binding | `f0ed46a1045e84a85c846b79f966f90383601131300e1430e48e4c1544e86e0c` |

The integration tests load these files, preserve their inputs and identities, and save through the current codec. Do not regenerate them using the latest writer.
