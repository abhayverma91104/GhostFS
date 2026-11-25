FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    g++ \
    clang \
    git \
    curl \
    ca-certificates \
    python3 \
    python3-pip \
    python3-venv \
    && curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY engine /app/engine
COPY backend /app/backend
COPY frontend /app/frontend
COPY scripts /app/scripts

RUN cmake -S /app/engine -B /app/engine/build && \
    cmake --build /app/engine/build -j4 && \
    ctest --test-dir /app/engine/build --output-on-failure

RUN python3 -m venv /app/backend/.venv && \
    /app/backend/.venv/bin/pip install --no-cache-dir -r /app/backend/requirements.txt && \
    /app/backend/.venv/bin/pytest /app/backend/tests -v

RUN rm -rf /app/frontend/node_modules && \
    npm --prefix /app/frontend install && \
    npm --prefix /app/frontend run build

EXPOSE 8000 5173
CMD ["/app/backend/.venv/bin/uvicorn", "app.main:app", "--app-dir", "/app/backend", "--host", "0.0.0.0", "--port", "8000"]
