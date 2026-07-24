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

  it('queries bridge information from the BZM-only endpoint', () => {
    service.getBridgeInfo().subscribe(info => {
      expect(info.version).toBe('1.2.3');
    });
    const request = http.expectOne(req =>
      req.url.endsWith('/api/system/bridge'));
    expect(request.request.method).toBe('GET');
    request.flush({
      available: true,
      versionQuerySupported: true,
      version: '1.2.3',
      protocolMajor: 1,
      protocolMinor: 0,
    });
  });

  it('uploads the bridge image as an unwrapped octet stream', () => {
    const image = new Blob(['bridge']);
    service.performBridgeUpdate(image).subscribe();
    const request = http.expectOne('/api/system/bridge/firmware');
    expect(request.request.method).toBe('POST');
    expect(request.request.headers.get('Content-Type'))
      .toBe('application/octet-stream');
    expect(request.request.body).toBe(image);
    request.flush({
      state: 'preparing', progress: 0, imageSize: image.size,
      running: true, manifestValidated: true, forceRequested: false,
      targetBoardVersion: 1002, imageVersion: '1.2.3',
      imageProtocolMajor: 1, imageProtocolMinor: 0,
      versionQuerySupported: false,
      currentVersion: null, error: null,
    });
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
