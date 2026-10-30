ARG BUILD_FROM
FROM $BUILD_FROM
RUN apk add --no-cache python3 py3-aiohttp
COPY app /app
# s6-overlay starts CMD with a clean environment; with-contenv brings SUPERVISOR_TOKEN back
CMD ["/usr/bin/with-contenv", "python3", "/app/hub.py"]
