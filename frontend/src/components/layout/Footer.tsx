import { Github, ExternalLink } from 'lucide-react';
import { useMemo } from 'react';

function Footer() {
  const siteName = useMemo(() => {
    const host = window.location.host;
    return host.replace(/^www\./, '');
  }, []);

  return (
    <footer className="mt-auto border-t border-[#1a1a1a] bg-[#0f0f0f]">
      <div className="max-w-6xl mx-auto px-4 sm:px-6 py-8">
        <div className="flex flex-col sm:flex-row items-center justify-between gap-4">
          <div className="text-center sm:text-left">
            <p className="text-sm text-[#a0a0a0]">
              <span className="text-white font-medium font-mono">{siteName}</span> - Bitcoin raw transaction broadcast service
            </p>
            <p className="text-xs text-[#666] mt-1">
              Specializing in non-standard transactions that other services reject
            </p>
          </div>

          <div className="flex items-center gap-4">
            <a
              href="https://github.com/8144225309/sendrawtx"
              target="_blank"
              rel="noopener noreferrer"
              className="flex items-center gap-2 text-sm text-[#a0a0a0] hover:text-white transition-colors"
            >
              <Github className="w-4 h-4" />
              <span>GitHub</span>
            </a>
            <a
              href="https://mempool.space"
              target="_blank"
              rel="noopener noreferrer"
              className="flex items-center gap-2 text-sm text-[#a0a0a0] hover:text-white transition-colors"
            >
              <ExternalLink className="w-4 h-4" />
              <span>mempool.space</span>
            </a>
          </div>
        </div>

        <div className="mt-6 pt-6 border-t border-[#1a1a1a] text-center">
          <p className="text-xs text-[#666]">
            Built with Bitcoin Core, Knots, and Libre Relay
          </p>
        </div>
      </div>
    </footer>
  );
}

export { Footer };
