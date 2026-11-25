# GhostFS REST API Reference (`/api/v1`)

- **Health**: `GET /api/v1/health`
- **Disks**: `GET /api/v1/disks`, `POST /api/v1/disks`, `GET /api/v1/disks/{name}`, `POST /api/v1/disks/{name}/format`, `DELETE /api/v1/disks/{name}`
- **Filesystem**: `GET /api/v1/disks/{name}/fs/tree`, `GET /stat`, `POST /mkdir`, `POST /files`, `POST /files/upload`, `GET /files/read`, `GET /files/download`, `POST /rename`, `DELETE /delete`
- **Allocator & Fragmentation**: `PATCH /api/v1/disks/{name}/policy`, `GET /blocks/map`, `GET /blocks/{id}`, `GET /blocks/extents`, `GET /fragmentation`, `POST /alloc-compare`
- **Recovery**: `POST /api/v1/disks/{name}/recovery/scan`, `POST /recovery/extract`, `GET /api/v1/recovery/artifacts/{id}`, `GET /download`, `GET /manifest`
- **Hex Viewer**: `GET /api/v1/disks/{name}/hex/blocks/{id}`, `GET /range`, `GET /files`, `GET /artifacts/{fname}`, `GET /structures`, `POST /interpret`, `POST /search`, `POST /analyze`, `POST /compare`
- **Corruption Lab**: `POST /api/v1/corruption/experiments`, `GET /experiments`, `GET /experiments/{id}`, `POST /inject`, `POST /check`, `POST /repair-plan`, `POST /repair`, `GET /comparison`, `GET /manifest`, `GET /download`, `DELETE /experiments/{id}?confirm=true`
- **Disk Scheduling**: `POST /api/v1/scheduling/simulate`, `POST /compare`, `POST /workloads/generate`, `POST /disks/{name}/scheduling/workloads/{files|recovery|metadata}`, `POST /allocation-compare`
- **Reports**: `GET /api/v1/reports/evidence`, `POST /preview`, `POST /generate`, `GET /`, `GET /{id}`, `GET /{id}/manifest`, `GET /{id}/download/{pdf|json}`, `GET /{id}/exports`, `GET /{id}/exports/{eid}`, `POST /{id}/verify`, `DELETE /{id}?confirm=true`
