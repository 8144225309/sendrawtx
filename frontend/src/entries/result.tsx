import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { Header } from '@/components/layout/Header';
import { Footer } from '@/components/layout/Footer';
import { TxResultPage } from '@/pages/TxResultPage';
import '@/styles/index.css';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <div className="min-h-screen flex flex-col bg-[#0f0f0f]">
      <Header />
      <main className="flex-1 flex flex-col">
        <TxResultPage />
      </main>
      <Footer />
    </div>
  </StrictMode>,
);
