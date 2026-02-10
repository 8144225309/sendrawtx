import { Routes, Route } from 'react-router-dom';
import { Header } from '@/components/layout/Header';
import { Footer } from '@/components/layout/Footer';
import { HomePage } from '@/pages/HomePage';
import { TxResultPage } from '@/pages/TxResultPage';
import { ApiDocsPage } from '@/pages/ApiDocsPage';
import { HealthPage } from '@/pages/HealthPage';
import { NotFoundPage } from '@/pages/NotFoundPage';
import { LogoShowcase } from '@/pages/LogoShowcase';

function App() {
  return (
    <div className="min-h-screen flex flex-col bg-[#0f0f0f]">
      <Header />
      <main className="flex-1 flex flex-col">
        <Routes>
          <Route path="/" element={<HomePage />} />
          <Route path="/tx/:txid" element={<TxResultPage />} />
          <Route path="/docs" element={<ApiDocsPage />} />
          <Route path="/health" element={<HealthPage />} />
          <Route path="/logos" element={<LogoShowcase />} />
          <Route path="*" element={<NotFoundPage />} />
        </Routes>
      </main>
      <Footer />
    </div>
  );
}

export default App;
