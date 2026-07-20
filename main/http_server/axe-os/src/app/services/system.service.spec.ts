import { TestBed } from '@angular/core/testing';

import { SystemApiService } from './system.service';
import { provideHttpClient } from '@angular/common/http';
import {
  HttpTestingController, provideHttpClientTesting
} from '@angular/common/http/testing';

describe('SystemApiService', () => {
  let service: SystemApiService;
  let http: HttpTestingController;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideHttpClient(), provideHttpClientTesting()]
    });
    service = TestBed.inject(SystemApiService);
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => http.verify());

  it('should be created', () => {
    expect(service).toBeTruthy();
  });

  it('sends production settings through the generated API', () => {
    const settings = {
      hostname: 'bitaxe-test',
      ssid: 'test-network',
      wifiPass: 'test-password',
    };

    service.updateSystem('', settings).subscribe();

    const request = http.expectOne(req =>
      req.url.endsWith('/api/system'));
    expect(request.request.method).toBe('PATCH');
    expect(request.request.body).toEqual(settings);
    request.flush({ status: 'success' });
  });

  it('sends remote production settings to the selected device URI', () => {
    const settings = {
      hostname: 'remote-bitaxe',
      ssid: 'remote-network',
      wifiPass: 'remote-password',
    };

    service.updateSystem('http://192.168.4.1', settings).subscribe();

    const request = http.expectOne('http://192.168.4.1/api/system');
    expect(request.request.method).toBe('PATCH');
    expect(request.request.body).toEqual(settings);
    request.flush({ status: 'success' });
  });
});
