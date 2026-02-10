import { Link } from 'react-router-dom';
import { Home, AlertCircle } from 'lucide-react';
import { Button, Card, CardContent } from '@/components/ui';

function NotFoundPage() {
  return (
    <div className="flex-1 flex items-center justify-center py-12">
      <Card className="max-w-md text-center">
        <CardContent>
          <AlertCircle className="w-16 h-16 text-[#f7931a] mx-auto mb-4" />
          <h1 className="text-3xl font-bold text-white mb-2">404</h1>
          <p className="text-[#a0a0a0] mb-6">
            The page you're looking for doesn't exist or has been moved.
          </p>
          <Link to="/">
            <Button>
              <Home className="w-4 h-4 mr-2" />
              Back to Home
            </Button>
          </Link>
        </CardContent>
      </Card>
    </div>
  );
}

export { NotFoundPage };
