import { ComponentFixture, TestBed } from '@angular/core/testing';

import { SwarmComponent } from './swarm.component';
import { ModalComponent } from '../modal/modal.component';
import { FileUploadModule } from 'primeng/fileupload';
import { InputGroupModule } from 'primeng/inputgroup';
import { ReactiveFormsModule } from '@angular/forms';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';

describe('SwarmComponent', () => {
  let component: SwarmComponent;
  let fixture: ComponentFixture<SwarmComponent>;

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [SwarmComponent, ModalComponent],
      imports: [FileUploadModule, InputGroupModule, ReactiveFormsModule],
      providers: [provideHttpClient(), provideToastr()]
    });
    fixture = TestBed.createComponent(SwarmComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('recognizes the Bitaxe 1002 fallback identity', () => {
    expect(component['deriveDeviceModel']({ boardVersion: '1002' })).toBe('Bonanza');
    expect(component['deriveSwarmColor']('Bonanza')).toBe('yellow');
  });

  it('uses advertised frequency options for low-frequency warnings', () => {
    const bzm = { frequency: 50, frequencyOptions: [50] };
    expect(component.getDeviceNotification(bzm)).toBeUndefined();

    bzm.frequency = 49;
    expect(component.getDeviceNotification(bzm)).toEqual({
      color: 'orange',
      msg: 'Frequency Low'
    });
  });
});
