# ProsperoEden — mises à jour du 29/10/2026 (build PS5 de test « g »)

Branche proposée : `sw2-ns2-test-20261029`. Build PS5 correspondante : eboot.bin sha256
`0b1070dca3c093557ae33f89e5bb807f4fd6b48a0c0a34e536ec655adb878097` (75 879 422 octets).

## Fichiers du port modifiés ou ajoutés (par rapport à l'arbre de départ)

| Fichier | Changement |
|---|---|
| `headless/main.cpp` | `/data/prosperoeden/debug-env.txt` (variables KEY=VALUE pour une session, ex. `EDEN_LOG_FILTER`, `EDEN_HOST_SVC_TRACE`) ; `EDEN_LOG_FILTER` aussi sur PS5 ; options de diagnostic pour la simulation Linux (12 Gio, sans CRAM). |
| `headless/mock_devices.cpp`, `headless/mock_devices.h` | Recréés (absents de l'instantané) : pad, utilisateur, audio et fichiers pour la simulation Linux, trace de crash, vidage du log sur SIGTERM. |
| `headless/inject.cmake` | Option `EDEN_HOST_RESEARCH_PROFILE` (couche recherche dans la build Linux). |
| `headless/research/direct-loader-prototype/research_execution_profile.h`, `research_frontend.cpp` | Profil mémoire strict et 12 Gio pour **tout jeu Switch 2** (ID 04…), plus seulement Mario Kart World. |
| `headless/research/direct-loader-prototype/direct_loader_targets.cmake` | Empreinte de `program_metadata.cpp` mise à jour (drapeau NPDM Switch 2). |
| `tools/build-headless-native.sh` | `EDEN_EXTRA_CMAKE_ARGS`. |
| `tools/prepare-vulkan-port.py` | Ancre DispatchCompute adaptée au code NS2. |
| `tools/native-startup-research/manifest.json`, `overlay/.../direct_loader_targets.cmake` | Alignés sur les changements ci-dessus. |
| `patches/eden/` (nouveau) | Patch Eden complet + scripts de build utilisés. |

## Patch Eden (`patches/eden/eden-67bada77-ns2-sw2-ps5.patch`, base Eden 67bada77)

- **NS2.patch intégré** : IGraphicsSupervisor (ISelfController 2000), IGpuSession/IGpuWorker, nvhost-tsg, classes Ampere, abaissement SM86 → Maxwell, QMD natif.
- **Chargement Switch 2** : dossier exefs (main + main.npdm), NPDM à ACID étendu, PAuth désactivé pour les processus Switch 2, tas jusqu'à 16 Gio (Pokopia demande 9,5 Gio), GetInfo 31–33.
- **GSV / VI2** : 1111/1112, Echo de syncpoint, sessions GPU/affichage, VI2 31/32 (vsync), 261–263.
- **GPU** : IGpuSession 201 (attache de l'espace d'adresses), VSMS (ioctl 0x13), vues d'espace d'adresses GPU (drapeaux 0 et 5).
- **Applet** : proxy natif (60/GetPerformanceFunctions) pour les attributs 0 et 2.
- **Gates Switch 2 généralisées** à tous les ID 04… (Mario Kart World, Welcome Tour, Pokopia).
- **Diagnostics** : `EDEN_GUEST_EXCEPTION` (stderr), traces SVC/IPC et QMD activables.

## Où nous en sommes

| Jeu | PS5 (console) | Simulation Linux |
|---|---|---|
| Mario Kart World | charge, init graphique, arrêt sur un objet vide (lecture 0x20) | même arrêt |
| Pokopia | charge, démarrage applet/compte, puis erreur CPU émulé (`assert !is_executing`, saut à 0) | va jusqu'aux commandes GPU ; bloque sur un QMD encore vide |
| Welcome Tour | exefs renvoyé, pas encore testé | init graphique, puis pointeurs invalides |

Aucune première image pour l'instant.
