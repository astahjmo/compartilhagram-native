FROM archlinux:base

# Desktop runtime and a headless compositor; deliberately no Qt, WebRTC,
# FFmpeg, PulseAudio or PipeWire packages to mask missing bundled libraries.
RUN pacman-key --init && pacman-key --populate archlinux \
    && pacman -Syu --noconfirm --needed \
      python weston dbus ttf-dejavu \
      libx11 libxcomposite libxdamage libxrandr libxfixes libxrender libxtst \
      libxcb libxkbcommon libglvnd mesa alsa-lib \
      fontconfig freetype2 harfbuzz fribidi zlib bzip2 expat gmp \
    && pacman -Scc --noconfirm

ARG TEST_UID=1000
ARG TEST_GID=1000
RUN (getent group "$TEST_GID" || groupadd --gid "$TEST_GID" tester) \
    && (getent passwd "$TEST_UID" || useradd --uid "$TEST_UID" --gid "$TEST_GID" --no-create-home tester)

COPY verify-appdir.py /opt/test/verify-appdir.py
COPY archlinux/smoke.py /opt/test/smoke.py
ENV PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["dbus-run-session", "--", "python3", "/opt/test/smoke.py"]
