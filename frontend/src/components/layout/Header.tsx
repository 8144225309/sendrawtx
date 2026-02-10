import { useEffect, useMemo } from 'react';
import { FileText, Activity, Zap } from 'lucide-react';

function Header() {
  const siteName = useMemo(() => {
    const host = window.location.host;
    return host.replace(/^www\./, '');
  }, []);

  useEffect(() => {
    document.title = `${siteName} - Bitcoin Transaction Broadcast`;
  }, [siteName]);

  const navLinks = [
    { to: '/', label: 'Broadcast', icon: Zap },
    { to: '/docs', label: 'API Docs', icon: FileText },
    { to: '/health', label: 'Status', icon: Activity },
  ];

  const pathname = window.location.pathname;

  const isActive = (path: string) => {
    if (path === '/') return pathname === '/';
    return pathname.startsWith(path);
  };

  return (
    <header className="sticky top-0 z-50 bg-[#0f0f0f]/95 backdrop-blur border-b border-[#1a1a1a]">
      <div className="max-w-6xl mx-auto px-4 sm:px-6">
        <div className="flex items-center justify-between h-16">
          <a href="/" className="flex items-center gap-2 group">
            <div className="w-14 h-14 flex items-center justify-center">
              <svg viewBox="0 0 100 100" className="w-full h-full">
                <polygon points="50,2 93,26 93,74 50,98 7,74 7,26" fill="none" stroke="#f7931a" strokeWidth="2" opacity="0.1" />
                <polygon points="50,8 87,29 87,71 50,92 13,71 13,29" fill="none" stroke="#f7931a" strokeWidth="2" opacity="0.2" />
                <polygon points="50,14 81,32 81,68 50,86 19,68 19,32" fill="none" stroke="#f7931a" strokeWidth="2" opacity="0.35" />
                <polygon points="50,20 75,35 75,65 50,80 25,65 25,35" fill="none" stroke="#f7931a" strokeWidth="2" opacity="0.55" />
                <polygon points="50,26 69,38 69,62 50,74 31,62 31,38" fill="none" stroke="#f7931a" strokeWidth="2.5" opacity="0.8" />
                <text x="50" y="58" textAnchor="middle" fontSize="20" fontWeight="bold" fill="#f7931a" fontFamily="Georgia, serif">TX</text>
              </svg>
            </div>
            <span className="text-lg font-bold text-white group-hover:text-[#f7931a] transition-colors font-mono">
              {siteName}
            </span>
          </a>

          <nav className="flex items-center gap-1">
            {navLinks.map(({ to, label, icon: Icon }) => (
              <a
                key={to}
                href={to}
                className={`
                  flex items-center gap-2 px-4 py-2 rounded-lg text-sm font-medium transition-colors
                  ${isActive(to)
                    ? 'bg-[#1a1a1a] text-white'
                    : 'text-[#a0a0a0] hover:text-white hover:bg-[#1a1a1a]'
                  }
                `}
              >
                <Icon className="w-4 h-4" />
                <span className="hidden sm:inline">{label}</span>
              </a>
            ))}
          </nav>
        </div>
      </div>
    </header>
  );
}

export { Header };
