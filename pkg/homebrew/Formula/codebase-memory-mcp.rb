class CodebaseMemoryMcp < Formula
  desc "Fast code intelligence engine for AI coding agents"
  homepage "https://github.com/Be90nia/codebase-memory-mcp-pro"
  version "0.10.9"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Be90nia/codebase-memory-mcp-pro/releases/download/v#{version}/codebase-memory-mcp-darwin-arm64.tar.gz"
      sha256 "9bedd7c772b4f505437e84605261fdf57a0ef89814fb7acf32c6e77798b1b1db"
    end
    on_intel do
      url "https://github.com/Be90nia/codebase-memory-mcp-pro/releases/download/v#{version}/codebase-memory-mcp-darwin-amd64.tar.gz"
      sha256 "f6e0c230001d38db553cf5f19e1ba1e7feb7af2ba4f23d039bd3f30bee27e136"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Be90nia/codebase-memory-mcp-pro/releases/download/v#{version}/codebase-memory-mcp-linux-arm64.tar.gz"
      sha256 "737c2a74c0721cd9458d01f8c66f343ccdc1510915d805f582b0f0d344ea78c2"
    end
    on_intel do
      url "https://github.com/Be90nia/codebase-memory-mcp-pro/releases/download/v#{version}/codebase-memory-mcp-linux-amd64.tar.gz"
      sha256 "55e7c5f3b699bf04a87f311a243c148f41ce8e0860eae16b81473f82eac379f0"
    end
  end

  def install
    bin.install "codebase-memory-mcp"
    # Third-party attribution bundle (present in archives since v0.8.1)
    doc.install "THIRD_PARTY_NOTICES.md" if File.exist?("THIRD_PARTY_NOTICES.md")
  end

  def caveats
    <<~EOS
      Run the following to configure your coding agents:
        codebase-memory-mcp install

      To tap this formula directly:
        brew tap deusdata/codebase-memory-mcp https://github.com/Be90nia/codebase-memory-mcp-pro
        brew install codebase-memory-mcp
    EOS
  end

  test do
    assert_match "codebase-memory-mcp", shell_output("#{bin}/codebase-memory-mcp --version")
  end
end
